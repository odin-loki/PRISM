#include "prism/settings.hpp"
#ifdef __linux__
#include <malloc.h>
#endif
#include "prism/astlint.hpp"
#include "prism/pipeline.hpp"

#include "prism/ai.hpp"
#include "prism/ai_proof.hpp"
#include "prism/ai_assist.hpp"
#include "prism/astlint.hpp"
#include "prism/cparse.hpp"
#include "prism/journal.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/polyglot.hpp"
#include "prism/conc.hpp"
#include "prism/sandbox.hpp"
#include "prism/scope.hpp"
#include "prism/shipdocs.hpp"
#include "prism/stages.hpp"
#include "prism/taxonomy.hpp"
#include "prism/verdict.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <format>
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

// One "rel \0 size \0 sha256" line per file under p (p itself when a file),
// skipping the scope's vendor/build dirs and `exclude`. Content hashes, not
// mtimes: an edit that keeps the size and mtime (cp -p, rsync -a, a
// checkout of another branch inside one second) must not replay old findings,
// and the pipeline reads every byte of these files anyway.
void digest_lines(const std::filesystem::path& p, const std::filesystem::path& exclude, const std::string& tag,
                  std::vector<std::string>& lines) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto one = [&](const fs::path& f, const std::string& rel) {
        std::error_code e2;
        auto size = fs::file_size(f, e2);
        auto h = ai::sha256_file(f);
        lines.push_back(tag + rel + '\0' + (e2 ? std::string("?") : std::to_string(size)) + '\0' +
                        (h.empty() ? std::string("unreadable") : h));
    };
    if (fs::is_regular_file(p, ec)) {
        one(p, p.filename().generic_string());
        return;
    }
    if (!fs::is_directory(p, ec)) {
        lines.push_back(tag + "<missing>");
        return;
    }
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) {
            std::error_code e3;
            if (scope::skip_dir(it->path().filename().string()) ||
                (!exclude.empty() && fs::equivalent(it->path(), exclude, e3)))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(e2)) continue;
        one(it->path(), it->path().lexically_relative(p).generic_string());
    }
}

const std::set<std::string>& exec_key_fields() {
    static const std::set<std::string> k{"allow_exec", "allow_unsandboxed", "sandbox"};
    return k;
}

// External tools in the run key: "tool:<name>" -> the resolved path or
// "missing", and the stages that use it. Installing (or removing, or
// pointing --tool at) a tool re-runs exactly those stages, so a NOTRUN row
// for a missing tool is not replayed once the tool is there. A tool upgraded
// in place at the same path is not seen (the digest is of the resolution).
struct ToolKey {
    std::string field;
    std::optional<std::filesystem::path> where;
    std::set<std::string> stages;  // empty: every stage (compilers, LLVM)
};

std::vector<ToolKey> tool_keys(const Config& cfg) {
    std::vector<ToolKey> out;
    auto add = [&](std::string name, std::optional<std::filesystem::path> where, std::set<std::string> stages) {
        out.push_back({"tool:" + std::move(name), std::move(where), std::move(stages)});
    };
    // The C/C++ toolchain feeds nearly every stage.
    add("cc", cfg.which({"gcc", "clang", "cc"}), {});
    add("llvm-clang", cfg.llvm_tool("clang"), {});
    add("llvm-clang++", cfg.llvm_tool("clang++"), {});
    add("llvm-opt", cfg.llvm_tool("opt"), {"pir", "conc", "fuzz", "execute"});
    add("llvm-lli", cfg.llvm_tool("lli"), {"pir"});
    add("bwrap", cfg.which({"bwrap"}), {});
    for (auto& [s, how] : exec_stages()) out.back().stages.insert(s);
    // Adapters (adapters.cpp, fuse.cpp, ltl.cpp, the optional stage table).
    add("cppcheck", cfg.which_adapter("cppcheck", {"cppcheck", "cppcheck.exe"}), {"cppcheck"});
    add("esbmc", cfg.which_adapter("esbmc", {"esbmc", "esbmc.exe"}), {"esbmc"});
    add("dafny", cfg.which_adapter("dafny", {"dafny", "dafny.exe"}), {"dafny"});
    add("afl-fuzz", cfg.which_adapter("afl-fuzz", {"afl-fuzz", "afl-fuzz.exe"}), {"fuzz", "optional"});
    add("strix", cfg.which_adapter("strix", {"strix", "strix.exe"}), {"ltl", "optional"});
    add("klee", cfg.which_adapter("klee", {"klee"}), {"optional"});
    add("frama-c", cfg.which_adapter("frama-c", {"frama-c", "frama-c.exe"}), {"optional"});
    add("infer", cfg.which_adapter("infer", {"infer"}), {"optional"});
    add("clang-tidy", cfg.which_adapter("clang-tidy", {"clang-tidy", "clang-tidy.exe"}), {"optional"});
    add("cbmc", cfg.which_adapter("cbmc", {"cbmc", "cbmc.exe"}), {"optional"});
    add("semgrep", cfg.which_adapter("semgrep", {"semgrep", "semgrep.exe"}), {"optional"});
    add("spatch", cfg.which_adapter("spatch", {"spatch", "spatch.exe"}), {"optional"});
    // Every polyglot linter / syntax checker (native checkers need none).
    for (const auto& c : polyglot::checks())
        for (const auto& t : c.tools)
            if (!t.native && !t.exes.empty()) add("polyglot/" + t.name, polyglot::resolve(t, cfg), {"polyglot"});
    return out;
}

}  // namespace

RunKey run_key(const Config& cfg) {
    namespace fs = std::filesystem;
    RunKey k;
    std::error_code ec;
    const auto root = fs::absolute(cfg.root, ec);
    std::vector<std::string> lines;
    digest_lines(cfg.root, cfg.out, "", lines);
    std::string req;
    for (const auto& r : cfg.requirements) {
        digest_lines(r, {}, "req:" + r.generic_string() + ":", lines);
        req += r.generic_string() + ";";
    }
    if (!cfg.contracts_approved.empty() && !path_within(cfg.contracts_approved, root))
        digest_lines(cfg.contracts_approved, {}, "approved:", lines);
    std::sort(lines.begin(), lines.end());
    std::string all;
    for (auto& l : lines) all += l + '\n';
    k.tree = sha256_hex(all);
    auto& f = k.fields;
    auto b = [](bool v) { return std::string(v ? "1" : "0"); };
    f["version"] = PRISM_VERSION;
    f["root"] = root.generic_string();
    f["pbsd"] = cfg.pbsd_root.generic_string();
    f["model"] = cfg.model;
    f["fpk"] = cfg.fpk.generic_string();
    f["timeout"] = std::format("{}", cfg.timeout);
    f["unwind"] = std::to_string(cfg.unwind);
    std::string props;
    for (auto& p : cfg.pir_only_props) props += p + ",";
    f["pir_only_props"] = props;
    f["memory_model"] = cfg.conc_tso ? "tso" : "sc";
    f["store_buffer"] = std::to_string(cfg.conc_store_buffer);
    f["fuzz_budget"] = std::format("{}", cfg.fuzz_budget);
    f["fuzz_iters"] = std::to_string(cfg.fuzz_iters);
    f["llm"] = b(cfg.llm);
    f["repair_rounds"] = std::to_string(cfg.repair_rounds);
    std::string tools;
    for (auto& [n, p] : cfg.tools) tools += n + "=" + p.generic_string() + ";";
    f["tools"] = tools;
    f["requirements"] = req;
    f["contracts_approved"] = cfg.contracts_approved.generic_string();
    f["strict_aliasing"] = b(cfg.strict_aliasing);
    f["pir_drafts"] = b(cfg.pir_drafts);
    f["fp_checks"] = b(cfg.fp_checks);
    f["certified"] = b(cfg.certified);
    f["data_model"] = cfg.data_model;
    f["llvm_bin"] = cfg.llvm_bin.generic_string();
    f["allow_exec"] = b(cfg.allow_exec);
    f["allow_unsandboxed"] = b(cfg.allow_unsandboxed);
    // Which jail ran the exec steps (only probed when they may run).
    f["sandbox"] = cfg.allow_exec ? sandbox::kind() : "";
    for (auto& t : tool_keys(cfg)) f[t.field] = t.where ? t.where->generic_string() : std::string("missing");
    return k;
}

std::set<std::string> tool_key_stages(const std::string& field) {
    // Which tool lookups exist does not depend on the config; resolve them
    // once with the defaults to read the stage sets.
    static const auto table = [] {
        std::map<std::string, std::set<std::string>> m;
        for (auto& t : tool_keys(Config{})) m[t.field] = t.stages;
        return m;
    }();
    if (auto it = table.find(field); it != table.end()) return it->second;
    return {};
}

std::optional<RunKey> load_run_key(const std::filesystem::path& out) {
    std::ifstream in(out / RUN_KEY_JSON, std::ios::binary);
    if (!in) return std::nullopt;
    try {
        auto j = nlohmann::json::parse(in);
        if (!j.is_object() || j.value("version", 0) != 1 || !j.contains("tree") || !j["tree"].is_string() ||
            !j.contains("fields") || !j["fields"].is_object())
            return std::nullopt;
        RunKey k;
        k.tree = j["tree"].get<std::string>();
        for (auto& [n, v] : j["fields"].items()) {
            if (!v.is_string()) return std::nullopt;
            k.fields[n] = v.get<std::string>();
        }
        return k;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void save_run_key(const std::filesystem::path& out, const RunKey& key) {
    std::filesystem::create_directories(out);
    nlohmann::json j = {{"version", 1}, {"tree", key.tree}, {"fields", key.fields}};
    std::ofstream(out / RUN_KEY_JSON, std::ios::binary) << j.dump(2);
}

RunKeyDiff diff_run_key(const RunKey& before, const RunKey& now) {
    RunKeyDiff d;
    bool all = false, exec = false;
    if (before.tree != now.tree) {
        all = true;
        d.why.push_back("files under the scan root changed");
    }
    std::set<std::string> names;
    for (auto& [n, v] : before.fields) names.insert(n);
    for (auto& [n, v] : now.fields) names.insert(n);
    for (auto& n : names) {
        auto a = before.fields.find(n), b = now.fields.find(n);
        if (a != before.fields.end() && b != now.fields.end() && a->second == b->second) continue;
        auto shown = [](auto it, auto end) { return it == end ? std::string("unset") : "'" + it->second + "'"; };
        d.why.push_back(n + " " + shown(a, before.fields.end()) + " -> " + shown(b, now.fields.end()));
        if (exec_key_fields().contains(n)) {
            exec = true;
        } else if (n.starts_with("tool:")) {
            auto st = tool_key_stages(n);
            if (st.empty()) all = true;
            d.rerun.insert(st.begin(), st.end());
        } else {
            all = true;
        }
    }
    if (all) {
        for (auto* p = STAGE_ORDER; *p; ++p) d.rerun.insert(*p);
    } else if (exec) {
        for (auto& [s, how] : exec_stages()) d.rerun.insert(s);
    }
    return d;
}

// Law 9 (docs/PLAN.md "Running on untrusted code"): what each stage may
// execute. Same table as prism/pipeline.py EXEC_STAGES.
//   none  = pure analysis / parse / compile-only (not listed)
//   whole = only runs scanned code; NOTRUN without --allow-exec
//   part  = the analysis half runs, the execute half is NOTRUN without it
const std::map<std::string, std::string>& exec_stages() {
    static const std::map<std::string, std::string> k{
        {"pbsd", "part"},       // imports the configured ParanoidBSD tools/verify modules
        {"sanitize", "whole"},  // compiles + runs `// prism: run` functions under ASan/UBSan/TSan
        {"optional", "part"},   // klee (native external calls), tree-local .cocci script rules
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

verdict::Score confidence_score(const RunReport& report) {
    auto n_fun = report.functions.size();
    // Empty scope: the product is 0, never n/a or skipped.
    if (n_fun == 0) return {0, 0, 0, 0};
    // The instrument that answers per function: pir. One record per
    // function: the first one the stage wrote for it.
    static const std::set<std::string> kFormalStages{"pir"};
    bool any_formal = false;
    std::map<std::string, std::vector<const Finding*>> by_fn;
    for (auto& s : report.stages) {
        if (!kFormalStages.contains(s.name)) continue;
        any_formal = true;
        std::set<std::string> seen;
        for (auto& f : s.findings) {
            auto k = f.file + "::" + (f.function ? *f.function : "");
            if (seen.insert(k).second) by_fn[k].push_back(&f);
        }
    }
    // Every parsed function is classified: visibility is n_fun / n_fun.
    int answered = 0, resolved = 0, attempted = 0;
    auto nonempty = [](const Finding* f, const char* key) {
        auto it = f->extra.find(key);
        return it != f->extra.end() && !it->second.empty();
    };
    auto resolves = [&](const Finding* f) {
        if (laws::is_proof(f->status) || f->status == laws::BOUNDED) return true;
        // A counterexample is the instrument's answer; a failure without one
        // needs a person. An empty oracle/read value is no answer.
        return f->status == laws::FAILED &&
               (!f->counterexample.empty() || nonempty(f, "oracle") || nonempty(f, "read"));
    };
    if (any_formal) {
        bool any_scalar = std::any_of(report.functions.begin(), report.functions.end(),
                                      [](const FunctionInfo& fn) { return fn.kind == "SCALAR" || fn.kind == "VOID"; });
        for (auto& fn : report.functions) {
            if (any_scalar && fn.kind != "SCALAR" && fn.kind != "VOID") continue;
            auto key = fn.file + "::" + fn.name;
            auto it = by_fn.find(key);
            // All NEEDS-HARNESS is the absence of a precondition, like POINTER,
            // not a missing answer.
            if (it != by_fn.end() && !it->second.empty() &&
                std::all_of(it->second.begin(), it->second.end(),
                            [](const Finding* f) { return f->status == laws::NEEDS_HARNESS; }))
                continue;
            ++attempted;
            if (it == by_fn.end()) continue;
            bool any_answer = false, any_resolved = false;
            for (auto* f : it->second) {
                if (!laws::is_answered(f->status)) continue;
                any_answer = true;
                if (resolves(f)) any_resolved = true;
            }
            if (any_answer) ++answered;
            if (any_resolved) ++resolved;
        }
    }
    // Law 5 through the verdict module (proofs/Prism/Verdict.lean `score`).
    auto n = static_cast<long>(n_fun);
    return verdict::score_counts(n, n, attempted, answered, resolved);
}

double round4(double x) {
    // The exact binary value rounded to 4 decimals, ties to even: the
    // correctly rounded decimal form, read back.
    auto s = std::format("{:.4f}", x);
    double parsed = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), parsed);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return x;
    return parsed;
}

void apply_confidence(RunReport& report) {
    auto sc = confidence_score(report);
    report.visibility = round4(sc.visibility);
    report.answer = round4(sc.answer);
    report.resolution = round4(sc.resolution);
    report.confidence = round4(sc.confidence);
    if (report.functions.empty()) {
        static const char* kEmpty = "confidence 0: no functions parsed (no data, not clean)";
        if (std::ranges::find(report.notes, kEmpty) == report.notes.end()) report.notes.push_back(kEmpty);
    }
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
    // Roadmap 3.2 / 8.3 / 6.4: the trusted base and the verdict definitions
    // are written next to this file (shipdocs.hpp).
    o << "Trusted base: [" << TRUSTED_BASE_FILE << "](" << TRUSTED_BASE_FILE
      << ") says what a proof in this report depends on. Every verdict links to its definition in ["
      << VERDICTS_FILE << "](" << VERDICTS_FILE << ").\n\n";
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
            if (auto a = verdict_anchor(f.status); !a.empty())
                o << "  ([" << f.status << "](" << VERDICTS_FILE << "#" << a << "))";
            o << "\n";
        }
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << o.str();
    write_shipped_docs(path.parent_path());
}

RunReport run_pipeline(const Config& requested) {
    // Law 9: --allow-exec runs scanned code only inside an isolating sandbox,
    // or with resource limits only when the user said so
    // (--allow-exec=unsandboxed). Otherwise the exec steps are NOTRUN with
    // the hint; every stage below sees the effective cfg.allow_exec.
    const auto gate = sandbox::exec_gate(requested.allow_exec, requested.allow_unsandboxed,
                                         requested.allow_exec ? sandbox::kind() : std::string());
    Config cfg = requested;
    cfg.allow_exec = gate.run;
    // The exec policy holds for this run only (stages without a Config read
    // it through sandbox::allowed()).
    sandbox::Policy exec_policy(gate.run, gate.held_no_sandbox);
    // Roadmap 4.2/9.6: model-assisted invariants, harnesses and explanations
    // see this run's config and log to <out>/ai_audit.jsonl.
    ai::Session ai_session(cfg);
    RunReport report;
    report.root = cfg.root.string();
    report.started = now_secs();
    if (!gate.note.empty()) report.notes.push_back(gate.note);
    std::filesystem::create_directories(cfg.out);
    const RunKey key = run_key(cfg);
    if (!cfg.resume) {
        journal_reset(cfg.out);
        save_run_key(cfg.out, key);
    }
    std::map<std::string, StageResult> resume;
    if (cfg.resume) {
        // stages.jsonl is the live log. report.json is only a fallback when
        // the log file is absent — a failed/partial jsonl must not revive
        // ok/NOTRUN rows from a previous complete report.json.
        const bool jsonl_present = journal_stages_present(cfg.out);
        if (jsonl_present) resume = journal_completed_ok(cfg.out);
        std::size_t malformed = 0;
        auto fns = journal_read_functions(cfg.out, &malformed);
        if (malformed)
            report.notes.push_back(std::string(FUNCTIONS_JSON) + ": " + std::to_string(malformed) +
                                   " malformed entries; classify rerun");
        if (auto old = RunReport::load(cfg.out / "report.json")) {
            // Python engine pipeline.py: report.json stages AND functions are fallbacks
            // only when stages.jsonl is absent. A present jsonl with no
            // functions.json must not revive stale report.json functions.
            if (!jsonl_present) {
                for (auto& s : old->stages)
                    if (s.status == "ok" || s.status == "NOTRUN") resume[s.name] = s;
                if (fns.empty() && !malformed) fns = old->functions;
            }
        }
        // The journal holds results for the inputs in run_key.json only.
        if (!resume.empty() || !fns.empty()) {
            RunKeyDiff diff;
            if (auto before = load_run_key(cfg.out)) {
                diff = diff_run_key(*before, key);
            } else {
                for (auto* p = STAGE_ORDER; *p; ++p) diff.rerun.insert(*p);
                diff.why.push_back(std::string("no readable ") + RUN_KEY_JSON +
                                   " beside the journal (older PRISM or removed)");
            }
            if (!diff.rerun.empty()) {
                const bool all = diff.rerun.size() >= std::size(STAGE_ORDER) - 1;
                std::string why;
                for (auto& w : diff.why) why += (why.empty() ? "" : "; ") + w;
                std::string which;
                if (!all)
                    for (auto& s : diff.rerun) which += (which.empty() ? "" : ", ") + s;
                report.notes.push_back("resume: " + (all ? std::string("every stage") : which) +
                                       " re-run, not replayed (" + why + ")");
                for (auto& s : diff.rerun) resume.erase(s);
                // A changed tree makes the parsed functions stale too.
                if (diff.rerun.contains("classify")) fns.clear();
                // Start the journal again so a later --resume cannot pick
                // up the stale rows; replayed stages are appended anew.
                journal_reset(cfg.out);
                if (!fns.empty()) journal_write_functions(cfg.out, fns);
            }
        }
        save_run_key(cfg.out, key);
        if (!fns.empty()) report.functions = fns;
        if (!resume.empty()) {
            auto src = jsonl_present ? (cfg.out / "stages.jsonl") : (cfg.out / "report.json");
            report.notes.push_back("resumed from " + src.string());
        }
    }
    auto sources = iter_sources(cfg.root);
    std::vector<FunctionInfo> functions = report.functions;
    // inventory and classify read the same files: parse each once.
    std::map<std::filesystem::path, std::vector<FunctionInfo>> parsed;
    auto functions_of = [&](const std::filesystem::path& p, const std::string& rel) {
        auto it = parsed.find(p);
        if (it == parsed.end()) it = parsed.emplace(p, extract_functions(p, rel)).first;
        return it->second;
    };

    auto emit = [&](StageResult rec) -> StageResult {
#ifdef __linux__
        // Hand freed heap back after each stage, so a stage's freed memory
        // is not counted against the next stage's memory budget.
        ::malloc_trim(0);
#endif
        report.stages.push_back(rec);
        journal_append_stage(cfg.out, rec);
        return rec;
    };

    // Stages that read earlier stages' rows (report.stages, pir's findings):
    // once any stage before them ran again, their journal row is stale too.
    static const std::set<std::string> kConsumers{"review", "fuzz", "execute", "repair", "unify"};
    std::vector<std::string> fresh;  // stages run (not replayed) in this run
    std::vector<std::string> dependent_reruns;
    auto stage = [&](const char* name, auto fn) -> StageResult {
        if (!cfg.want(name)) {
            return emit(StageResult{name, "skipped", "excluded by --stage/--skip", 0, 0, {}, 0, {}});
        }
        if (auto it = resume.find(name); it != resume.end() &&
            (it->second.status == "ok" || it->second.status == "NOTRUN") &&
            !(std::string(name) == "classify" && functions.empty())) {
            if (!(kConsumers.contains(name) && !fresh.empty())) return emit(it->second);
            dependent_reruns.push_back(name);
        }
        if (cfg.resume) fresh.push_back(name);
        double t0 = now_secs();
        std::vector<Finding> findings;
        try {
            findings = fn();
        } catch (const std::exception& ex) {
            return emit(StageResult{name, "failed", ex.what(), t0, now_secs() - t0, {}, 0, {}});
        } catch (...) {
            // Anything else a stage throws is still written down (Law 7), not a
            // terminate with no report.
            return emit(StageResult{name, "failed", "stage threw a non-standard exception", t0,
                                    now_secs() - t0, {}, 0, {}});
        }
        std::string status = "ok", install, detail;
        if (!findings.empty()) {
            bool all_nr = true;
            // A NOTRUN row for a sub-layer (extra.layer, e.g. the Clang-AST
            // lints) does not make a stage whose main body ran NOTRUN.
            for (auto& f : findings)
                if (f.status != laws::NOTRUN || astlint::is_layer_row(f)) all_nr = false;
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
        // A mistyped root is not an empty, clean tree (Laws 1 and 7). The CLI
        // refuses it; library and GUI callers get this row.
        if (std::error_code ec; !std::filesystem::exists(cfg.root, ec)) {
            out.push_back(Finding{"inventory", std::string(laws::ERROR), "", std::nullopt,
                                  std::nullopt, "", "scan root does not exist: " + cfg.root.string(),
                                  std::string(laws::STRENGTH_FINDS), "", "", {}});
            return out;
        }
        for (auto& p : sources) {
            auto rel = rel_of(p, cfg.root);
            auto fns = functions_of(p, rel);
            auto gaps = parse_gap_findings(p, rel);
            out.insert(out.end(), gaps.begin(), gaps.end());
            if (fns.empty()) {
                if (is_tu_ext(p.extension().string())) {
                    out.push_back(Finding{"inventory", std::string(laws::ERROR), rel, std::nullopt,
                                          std::nullopt, "EMPTY-TU",
                                          "no functions parsed (not a clean unit)",
                                          std::string(laws::STRENGTH_FINDS), "", "", {}});
                }
                continue;
            }
            out.push_back(Finding{"inventory", std::string(laws::CLEAN), rel, std::nullopt,
                                  std::nullopt, "", "translation unit",
                                  std::string(laws::STRENGTH_FINDS), "", "", {}});
        }
        // Law 7: a skipped vendor/build directory holding sources is written
        // down, not skipped quietly (prism/scope.hpp).
        for (auto& sk : scope::skipped_dirs(cfg.root, is_known_source)) {
            // PRISM's own --out (run_key.json, report.json) is not a skipped
            // vendor directory of the scanned tree.
            if (std::error_code eq; std::filesystem::equivalent(cfg.root / sk.dir, cfg.out, eq)) continue;
            Finding f{"inventory", std::string(laws::UNKNOWN), "", std::nullopt, std::nullopt, "",
                      scope::skipped_message(sk.dir, sk.files), std::string(laws::STRENGTH_FINDS),
                      "", "", {}};
            f.extra["skipped"] = sk.dir;
            f.extra["files"] = std::to_string(sk.files);
            out.push_back(f);
        }
        return out;
    });

    stage("classify", [&] {
        functions.clear();
        std::vector<Finding> out;
        for (auto& p : sources) {
            auto rel = rel_of(p, cfg.root);
            auto fns = functions_of(p, rel);
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
                out.push_back(f);
            }
        }
        report.functions = functions;
        journal_write_functions(cfg.out, functions);
        return out;
    });

    auto src_root = std::filesystem::is_directory(cfg.root) ? cfg.root : cfg.root.parent_path();
    // Regex lints + the Clang-AST lint layer (roadmap 2.8, src/prism/astlint.cpp).
    stage("lints", [&] { return astlint::run_lints_ast(sources, src_root, cfg); });
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
    stage("contracts", [&] {
        auto out = prove_contracts(functions, cfg.unwind);
        if (cfg.llm) {
            // Model-proposed Dafny-style specs: hypotheses beside the proofs.
            std::vector<FunctionInfo> fns;
            for (const auto& f : functions)
                if (f.kind == "SCALAR" || f.kind == "VOID") fns.push_back(f);
            auto specs = dafny_specs(fns, 3, cfg);
            out.insert(out.end(), specs.begin(), specs.end());
        }
        return out;
    });
    stage("wp", [&] { return run_wp(functions, cfg.unwind); });
    auto pir_rec = stage("pir", [&] { return pir::run_pir(sources, cfg); });
    stage("conc", [&] { return conc::run_conc(sources, cfg); });
    stage("harness", [&] { return run_harness(functions, cfg.unwind); });
    // Roadmap 9.2/9.3/4.2: vacuity audit, approved/drafted contracts, model
    // assumption audit, proof store and PROOF-REGRESSION (src/prism/ai/review.cpp).
    stage("review", [&] { return ai::run_review(functions, report.stages, cfg); });
    stage("concolic", [&] { return run_concolic(functions, 32); });
    stage("fuzz", [&] {
        return exec_gate_note(
            "fuzz",
            run_fuse(functions, pir_rec.findings, src_root, cfg.fuzz_budget, cfg.fuzz_iters, cfg.llm, cfg),
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
        // Unreadable directories are passed over, not a failed stage; the
        // skipped vendor/build directories are skipped here as everywhere.
        std::vector<std::filesystem::path> specs;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 src_root, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2)) {
                if (scope::skip_dir(it->path().filename().string())) it.disable_recursion_pending();
                continue;
            }
            if (it->path().extension() == ".ltl") specs.push_back(it->path());
        }
        std::sort(specs.begin(), specs.end());
        return run_ltl(functions, specs, cfg);
    });
    stage("llm", [&] {
        if (!cfg.llm) {
            return std::vector<Finding>{{"llm", std::string(laws::NOTRUN), "", std::nullopt,
                                         std::nullopt, "", "--no-llm", std::string(laws::STRENGTH_READS),
                                         "", "", {}}};
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
                                         std::nullopt, "", "--no-llm", std::string(laws::STRENGTH_READS),
                                         "", "", {}}};
        }
        for (auto& s : report.stages)
            for (auto& f : s.findings)
                if ((f.status == laws::FAILED || f.status == laws::CRASH) && !f.file.empty()) {
                    auto out = rlef_repair(f, cfg);
                    // Roadmap 9.3: counterexample explanation + PIR-verified fix
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
                                     std::string(laws::STRENGTH_READS), "", "", {}}};
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

    if (!dependent_reruns.empty()) {
        std::string list;
        for (auto& s : dependent_reruns) list += (list.empty() ? "" : ", ") + s;
        report.notes.push_back("resume: " + list + " re-run, not replayed (they read rows of stages that ran again)");
    }
    laws::audit_report(report);
    apply_confidence(report);
    report.save(cfg.out / "report.json");
    write_report_md(report, cfg.out / "report.md");
    // Roadmap 9.3 triage: triage.json + the report.md "Clusters" section.
    // Ordering only: the report (and so every status) is const here.
    // PRISM_TRIAGE=0 switches it off (docs/AI.md: its measured value).
    if (settings::triage().get()) ai::write_triage(report, cfg.out);
    write_sarif(report, cfg.out / "report.sarif");
    return report;
}

}  // namespace prism
