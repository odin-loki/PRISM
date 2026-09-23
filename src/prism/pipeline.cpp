#include "prism/pipeline.hpp"

#include "prism/ai.hpp"
#include "prism/cparse.hpp"
#include "prism/journal.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/sandbox.hpp"
#include "prism/scope.hpp"
#include "prism/stages.hpp"
#include "prism/taxonomy.hpp"
#include "prism/verdict.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace prism {
namespace {

double now_secs() {
    using clock = std::chrono::system_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::string rel_of(const std::filesystem::path& p, const std::filesystem::path& root) {
    std::error_code ec;
    if (std::filesystem::is_directory(root)) {
        auto r = std::filesystem::relative(p, root, ec);
        if (!ec) return r.generic_string();
    }
    return p.filename().string();
}

}  // namespace

// Law 9 (docs/PLAN.md "Running on untrusted code"): what each stage may
// execute. Same table as prism/pipeline.py EXEC_STAGES.
//   none  = pure analysis / parse / compile-only (not listed)
//   whole = only runs scanned code; NOTRUN without --allow-exec
//   part  = the analysis half runs, the execute half is NOTRUN without it
const std::map<std::string, std::string>& exec_stages() {
    static const std::map<std::string, std::string> k{
        {"pbsd", "part"},       // imports the configured ParanoidBSD tools/verify modules
        {"sanitize", "whole"},  // compiles + runs `// prism: run` functions under ASan/UBSan/TSan
        {"optional", "part"},   // klee (native external calls), .cocci script rules
        {"pir", "part"},        // Clang->PIR->Z3 runs; lli translation validation does not
        {"polyglot", "part"},   // perl -c, cargo clippy, eslint (PgTool::executes)
        {"fuzz", "part"},       // concrete oracle runs; compiled harness / AFL++ / libFuzzer do not
        {"diff", "whole"},      // compiles + runs both functions
        {"rapid", "part"},      // interpreter runs; gcc fallback does not
        {"muttest", "part"},    // interpreter runs; gcc fallback does not
        {"execute", "part"},    // concrete cex replay runs; LLM-written C does not
        {"repair", "whole"},    // compiles + runs LLM-written candidates
    };
    return k;
}

// A "part" stage that held back its execute half says so once (Law 7).
std::vector<Finding> exec_gate_note(const std::string& stage, std::vector<Finding> findings,
                                    std::string_view what) {
    bool marked = false, noted = false;
    for (const auto& f : findings) {
        if (auto it = f.extra.find("exec"); it != f.extra.end() && it->second == laws::NOTRUN)
            marked = true;
        if (auto it = f.extra.find("reason");
            f.status == laws::NOTRUN && it != f.extra.end() && it->second == sandbox::EXEC_REASON)
            noted = true;
    }
    if (marked && !noted) findings.push_back(sandbox::exec_notrun(stage, what));
    return findings;
}

void apply_confidence(RunReport& report) {
    auto n_fun = report.functions.size();
    if (n_fun == 0) {
        report.visibility = report.answer = report.resolution = report.confidence = 0;
        static const char* kEmpty = "confidence 0: no functions parsed (no data, not clean)";
        if (std::find(report.notes.begin(), report.notes.end(), kEmpty) == report.notes.end())
            report.notes.push_back(kEmpty);
        return;
    }
    const StageResult* bmc = nullptr;
    for (auto& s : report.stages)
        if (s.name == "bmc") bmc = &s;
    // Every parsed function is classified: visibility is n_fun / n_fun.
    int answered = 0, resolved = 0, attempted = 0;
    if (bmc) {
        std::map<std::string, std::vector<const Finding*>> by_fn;
        for (auto& f : bmc->findings) {
            by_fn[(f.file + "::" + (f.function ? *f.function : ""))].push_back(&f);
        }
        std::vector<const FunctionInfo*> scalar;
        for (auto& fn : report.functions)
            if (fn.kind == "SCALAR" || fn.kind == "VOID") scalar.push_back(&fn);
        auto& consider = scalar.empty() ? report.functions : [&]() -> const std::vector<FunctionInfo>& {
            return report.functions;
        }();
        for (auto& fn : consider) {
            if (fn.kind != "SCALAR" && fn.kind != "VOID" && !scalar.empty()) continue;
            auto key = fn.file + "::" + fn.name;
            auto it = by_fn.find(key);
            if (it != by_fn.end() && !it->second.empty() &&
                it->second[0]->status == laws::NEEDS_HARNESS)
                continue;
            ++attempted;
            if (it == by_fn.end() || it->second.empty()) continue;
            auto st = it->second[0]->status;
            if (laws::is_answered(st)) {
                ++answered;
                if (laws::is_proof(st))
                    ++resolved;
                else if (st == laws::FAILED) {
                    auto* f0 = it->second[0];
                    if (!f0->counterexample.empty() || f0->extra.contains("oracle") || f0->extra.contains("read"))
                        ++resolved;
                } else if (st == laws::BOUNDED)
                    ++resolved;
            }
        }
    }
    // Law 5 through the verdict module (proofs/Prism/Verdict.lean `score`).
    auto n = static_cast<long>(n_fun);
    auto sc = verdict::score_counts(n, n, attempted, answered, resolved);
    report.visibility = std::round(sc.visibility * 10000.0) / 10000.0;
    report.answer = std::round(sc.answer * 10000.0) / 10000.0;
    report.resolution = std::round(sc.resolution * 10000.0) / 10000.0;
    report.confidence = std::round(sc.confidence * 10000.0) / 10000.0;
}

std::vector<Finding> llm_forced_reads(std::vector<Finding> findings) {
    static const std::set<std::string> keep{
        std::string(laws::NOTRUN), std::string(laws::ERROR), std::string(laws::TIMEOUT),
        std::string(laws::HYPOTHESIS), std::string(laws::READS)};
    for (auto& f : findings) {
        f.stage = "llm";
        f.strength = std::string(laws::STRENGTH_READS);
        if (!keep.contains(f.status)) f.status = std::string(laws::HYPOTHESIS);
    }
    return findings;
}

void write_report_md(const RunReport& report, const std::filesystem::path& path) {
    std::ostringstream o;
    o << "# PRISM report\n\n";
    o << "root: `" << report.root << "`\n\n";
    o << "| visibility | answer | resolution | **confidence** |\n";
    o << "|---|---|---|---|\n";
    o << "| " << report.visibility << " | " << report.answer << " | " << report.resolution
      << " | **" << report.confidence << "** |\n\n";
    o << "Confidence is a product. 0 means no data, not clean.\n\n";
    o << "## Stages\n\n| stage | status | records | seconds | note |\n|---|---|---:|---:|---|\n";
    for (auto& s : report.stages) {
        auto note = s.detail.empty() ? s.install : s.detail;
        o << "| " << s.name << " | " << s.status << " | " << s.records << " | "
          << std::fixed << std::setprecision(2) << s.elapsed << " | " << note << " |\n";
    }
    o << "\n## Findings\n\n";
    for (auto& s : report.stages) {
        for (auto& f : s.findings) {
            // UNKNOWN/TIMEOUT stay in report.md (the Python engine pipeline.py). Only the
            // inventory/classify/unify noise lines are dropped.
            if ((f.status == laws::NOTRUN || f.status == laws::CLEAN) &&
                (s.name == "inventory" || s.name == "classify" || s.name == "unify"))
                continue;
            // Empty location/function/class parts are left out (a summary
            // row has no file); report.json keeps every field. pipeline.py _md_finding.
            o << "- `" << f.status << "` **" << s.name << "**";
            if (!f.file.empty())
                o << " " << (f.line ? f.file + ":" + std::to_string(*f.line) : f.file);
            if (f.function && !f.function->empty()) o << " `" << *f.function << "`";
            if (!f.cls.empty()) o << " " << f.cls;
            o << " — " << f.message;
            if (!f.counterexample.empty()) o << "  cex `" << f.counterexample << "`";
            o << "\n";
        }
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << o.str();
}

RunReport run_pipeline(const Config& cfg) {
    // Law 9: the exec policy holds for this run only (stages without a
    // Config read it through sandbox::allowed()).
    sandbox::Policy exec_policy(cfg.allow_exec);
    // Roadmap 4.2/9.6: model-assisted invariants, harnesses and explanations
    // see this run's config and log to <out>/ai_audit.jsonl.
    ai::Session ai_session(cfg);
    RunReport report;
    report.root = cfg.root.string();
    report.started = now_secs();
    std::filesystem::create_directories(cfg.out);
    if (!cfg.resume) journal_reset(cfg.out);
    std::map<std::string, StageResult> resume;
    if (cfg.resume) {
        // stages.jsonl is the live log. report.json is only a fallback when
        // the log file is absent — a failed/partial jsonl must not revive
        // ok/NOTRUN rows from a previous complete report.json.
        const bool jsonl_present = journal_stages_present(cfg.out);
        if (jsonl_present) resume = journal_completed_ok(cfg.out);
        auto fns = journal_read_functions(cfg.out);
        if (auto old = RunReport::load(cfg.out / "report.json")) {
            // Python engine pipeline.py: report.json stages AND functions are fallbacks
            // only when stages.jsonl is absent. A present jsonl with no
            // functions.json must not revive stale report.json functions.
            if (!jsonl_present) {
                for (auto& s : old->stages)
                    if (s.status == "ok" || s.status == "NOTRUN") resume[s.name] = s;
                if (fns.empty()) fns = old->functions;
            }
        }
        if (!fns.empty()) report.functions = fns;
        if (!resume.empty()) {
            auto src = jsonl_present ? (cfg.out / "stages.jsonl") : (cfg.out / "report.json");
            report.notes.push_back("resumed from " + src.string());
        }
    }
    auto sources = iter_sources(cfg.root);
    std::vector<FunctionInfo> functions = report.functions;

    auto emit = [&](StageResult rec) -> StageResult {
        report.stages.push_back(rec);
        journal_append_stage(cfg.out, rec);
        return rec;
    };

    auto stage = [&](const char* name, auto fn) -> StageResult {
        if (!cfg.want(name)) {
            return emit(StageResult{name, "skipped", "excluded by --stage/--skip"});
        }
        if (auto it = resume.find(name); it != resume.end() &&
            (it->second.status == "ok" || it->second.status == "NOTRUN") &&
            !(std::string(name) == "classify" && functions.empty())) {
            return emit(it->second);
        }
        double t0 = now_secs();
        std::vector<Finding> findings;
        try {
            findings = fn();
        } catch (const std::exception& ex) {
            return emit(StageResult{name, "failed", ex.what(), t0, now_secs() - t0});
        }
        std::string status = "ok", install, detail;
        if (!findings.empty()) {
            bool all_nr = true;
            for (auto& f : findings)
                if (f.status != laws::NOTRUN) all_nr = false;
            if (all_nr) {
                status = "NOTRUN";
                std::vector<std::string> inst, det;
                for (auto& f : findings) {
                    if (auto it = f.extra.find("install"); it != f.extra.end() && !it->second.empty())
                        inst.push_back(it->second);
                    det.push_back(f.stage + ": " + f.message);
                    if (det.size() >= 12) break;
                }
                for (auto& i : inst) {
                    if (install.find(i) == std::string::npos) {
                        if (!install.empty()) install += "; ";
                        install += i;
                    }
                }
                for (auto& d : det) {
                    if (!detail.empty()) detail += "; ";
                    detail += d;
                }
            }
        }
        return emit(StageResult{name, status, detail, t0, now_secs() - t0, findings,
                                static_cast<int>(findings.size()), install});
    };

    stage("inventory", [&] {
        std::vector<Finding> out;
        for (auto& p : sources) {
            auto rel = rel_of(p, cfg.root);
            auto fns = extract_functions(p, rel);
            auto gaps = parse_gap_findings(p, rel);
            out.insert(out.end(), gaps.begin(), gaps.end());
            if (fns.empty()) {
                if (is_tu_ext(p.extension().string())) {
                    out.push_back(Finding{"inventory", std::string(laws::ERROR), rel, std::nullopt,
                                          std::nullopt, "EMPTY-TU",
                                          "no functions parsed (not a clean unit)",
                                          std::string(laws::STRENGTH_FINDS)});
                }
                continue;
            }
            out.push_back(Finding{"inventory", std::string(laws::CLEAN), rel, std::nullopt,
                                  std::nullopt, "", "translation unit",
                                  std::string(laws::STRENGTH_FINDS)});
        }
        // Law 7: a skipped vendor/build directory holding sources is written
        // down, not skipped quietly (prism/scope.hpp).
        for (auto& sk : scope::skipped_dirs(cfg.root, is_known_source)) {
            Finding f{"inventory", std::string(laws::UNKNOWN), "", std::nullopt, std::nullopt, "",
                      scope::skipped_message(sk.dir, sk.files), std::string(laws::STRENGTH_FINDS)};
            f.extra["skipped"] = sk.dir;
            f.extra["files"] = std::to_string(sk.files);
            out.push_back(std::move(f));
        }
        return out;
    });

    stage("classify", [&] {
        functions.clear();
        std::vector<Finding> out;
        for (auto& p : sources) {
            auto rel = rel_of(p, cfg.root);
            auto fns = extract_functions(p, rel);
            functions.insert(functions.end(), fns.begin(), fns.end());
            for (auto& fn : fns) {
                Finding f;
                f.stage = "classify";
                f.status = fn.kind;
                f.file = rel;
                f.function = fn.name;
                f.line = fn.line;
                f.cls = fn.kind;
                f.message = fn.signature;
                f.strength = std::string(laws::STRENGTH_FINDS);
                f.extra["static"] = fn.is_static ? "true" : "false";
                out.push_back(std::move(f));
            }
        }
        report.functions = functions;
        journal_write_functions(cfg.out, functions);
        return out;
    });

    auto src_root = std::filesystem::is_directory(cfg.root) ? cfg.root : cfg.root.parent_path();
    stage("lints", [&] { return run_lints(sources, src_root, cfg.jobs); });
    stage("taint", [&] { return run_taint(functions); });
    stage("thread", [&] { return run_thread(functions); });
    stage("interval", [&] { return run_interval(functions); });
    stage("warnings", [&] { return run_compiler(sources, cfg); });
    stage("cppcheck", [&] { return run_cppcheck(sources, cfg); });
    stage("pbsd", [&] { return run_pbsd_lints(sources, cfg); });
    stage("sanitize", [&] { return run_sanitize(sources, cfg); });
    stage("optional", [&] { return run_optional_tools(sources, cfg); });
    stage("polyglot", [&] { return run_polyglot(cfg.root, cfg); });
    stage("esbmc", [&] { return run_esbmc(sources, cfg); });
    stage("dafny", [&] { return run_dafny(sources, cfg); });
    stage("contracts", [&] { return prove_contracts(functions, cfg.unwind); });
    stage("wp", [&] { return run_wp(functions, cfg.unwind); });
    auto bmc_rec = stage("bmc", [&] { return run_bmc(inline_static(functions), cfg.unwind); });
    stage("pir", [&] { return pir::run_pir(sources, cfg); });
    stage("harness", [&] { return run_harness_bmc(functions, cfg.unwind); });
    stage("concolic", [&] { return run_concolic(functions, 32); });
    stage("fuzz", [&] {
        return exec_gate_note(
            "fuzz",
            run_fuse(functions, bmc_rec.findings, src_root, cfg.fuzz_budget, cfg.fuzz_iters, cfg.llm),
            "fuzz (compiled harness, AFL++, libFuzzer)");
    });
    stage("diff", [&] { return run_diff(functions, src_root); });
    stage("rapid", [&] {
        return exec_gate_note("rapid", run_rapid(functions, 64), "rapid (gcc fallback harness)");
    });
    stage("muttest", [&] {
        return exec_gate_note("muttest", run_muttest(functions, 32), "muttest (gcc fallback harness)");
    });
    stage("ltl", [&] {
        std::vector<std::filesystem::path> specs;
        if (std::filesystem::exists(src_root)) {
            for (auto& p : std::filesystem::recursive_directory_iterator(src_root)) {
                if (p.path().extension() == ".ltl") specs.push_back(p.path());
            }
        }
        std::sort(specs.begin(), specs.end());
        return run_ltl(functions, specs);
    });
    stage("llm", [&] {
        if (!cfg.llm) {
            return std::vector<Finding>{{"llm", std::string(laws::NOTRUN), "", std::nullopt,
                                         std::nullopt, "", "--no-llm", std::string(laws::STRENGTH_READS)}};
        }
        return llm_forced_reads(hypothesize(functions, 4, cfg));
    });
    stage("execute", [&] {
        std::vector<Finding> fails;
        for (auto& s : report.stages)
            for (auto& f : s.findings)
                if ((f.status == laws::FAILED || f.status == laws::CRASH) && f.function &&
                    (f.counterexample.find('=') != std::string::npos))
                    fails.push_back(f);
        return execute_cex(fails, functions, cfg);
    });
    stage("repair", [&] {
        if (!cfg.llm) {
            return std::vector<Finding>{{"repair", std::string(laws::NOTRUN), "", std::nullopt,
                                         std::nullopt, "", "--no-llm", std::string(laws::STRENGTH_READS)}};
        }
        for (auto& s : report.stages)
            for (auto& f : s.findings)
                if ((f.status == laws::FAILED || f.status == laws::CRASH) && !f.file.empty()) {
                    auto out = rlef_repair(f, cfg);
                    // Roadmap 9.3: counterexample explanation + BMC-verified fix
                    // for up to 4 FAILED findings (HYPOTHESIS / READS; one NOTRUN
                    // row without a model). Never a verdict change.
                    int explained = 0;
                    for (auto& s2 : report.stages)
                        for (auto& g : s2.findings) {
                            if (g.status != laws::FAILED || !g.function || g.file.empty() || explained >= 4)
                                continue;
                            auto ex = ai::explain_failed(g, cfg);
                            ++explained;
                            bool notrun = !ex.empty() && ex[0].status == laws::NOTRUN;
                            out.insert(out.end(), ex.begin(), ex.end());
                            if (notrun) explained = 4;  // no model: say so once
                        }
                    return out;
                }
        return std::vector<Finding>{{"repair", std::string(laws::NOTRUN), "", std::nullopt,
                                     std::nullopt, "", "nothing to repair",
                                     std::string(laws::STRENGTH_READS)}};
    });
    // Verdict audit (docs/VERDICTS.md): before unify so taxonomy and
    // confidence only see admitted verdicts; again after, for unify itself.
    laws::audit_report(report);
    stage("unify", [&] {
        auto rows = coverage_from_report(report);
        nlohmann::json j = nlohmann::json::array();
        int covered = 0, gaps = 0;
        std::vector<std::string> gap_ids;
        for (auto& r : rows) {
            nlohmann::json seen = nlohmann::json::object();
            for (auto& [k, v] : r.seen) seen[k] = v;
            j.push_back({{"id", r.id}, {"name", r.name}, {"cwe", r.cwe}, {"seen", seen},
                         {"best", r.best}, {"verdict", r.verdict}});
            if (r.verdict == "COVERED") ++covered;
            if (r.verdict == "GAP") {
                ++gaps;
                gap_ids.push_back(r.id);
            }
        }
        std::ofstream(cfg.out / "taxonomy.json") << j.dump(2);
        Finding f;
        f.stage = "unify";
        f.status = std::string(laws::CLEAN);
        f.message = "taxonomy " + std::to_string(covered) + "/" + std::to_string(rows.size()) +
                    " COVERED, " + std::to_string(gaps) + " GAP (not a proof)";
        f.strength = std::string(laws::STRENGTH_FINDS);
        std::string g;
        for (auto& id : gap_ids) {
            if (!g.empty()) g += ",";
            g += id;
        }
        f.extra["gaps"] = g;
        f.extra["not_a_proof"] = "true";
        return std::vector<Finding>{f};
    });

    laws::audit_report(report);
    apply_confidence(report);
    report.save(cfg.out / "report.json");
    write_report_md(report, cfg.out / "report.md");
    write_sarif(report, cfg.out / "report.sarif");
    return report;
}

}  // namespace prism
