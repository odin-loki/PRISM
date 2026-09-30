// prism-qa conformance: the conformance suite runner (roadmap 2.7) and the
// release gate (roadmap 6.1).
//
// Runs PRISM on tests/conformance/ (in-house tasks, a pinned SV-COMP subset,
// concurrency programs, a curated ESBMC C++ subset, the libc model contract
// harnesses and, optionally, the NIST Juliet C/C++ subsets fetched with
// --fetch-juliet) and computes, per verdict stage:
//
//   SOUNDNESS     wrong proofs: a PROVED / PROVED-UNBOUNDED / PROVED-ASSUMING /
//                 PROVED-CERTIFIED verdict on a function whose task says a
//                 violation exists. Must be 0; any wrong proof exits 1.
//   COMPLETENESS  share of `true` functions proved.
//   DETECTION     share of `false` functions refuted with FAILED and a
//                 counterexample that replays: the task is compiled with
//                 -fsanitize=undefined,address and run on the counterexample
//                 inputs (inside bwrap when available); the sanitizer must fire.
//   FALSE ALARMS  FAILED on a `true` function.
//
// With --certified the pir stage is run a second time with `prism --certified`
// and scored as the verdict stage `pir-certified` (roadmap 3.2). The report then
// also says how many loop-free `true` functions became PROVED-CERTIFIED (the
// roadmap 3 exit criterion: all of them) and why the others did not.
//
// Task format (tests/conformance/prism/**.yml, one sidecar per source file):
//
//   format_version: 1
//   input_files: add_false.c
//   language: C            # or C++
//   property: no-overflow  # no-overflow | no-div0 | no-shift-ub | no-oob |
//                          # no-null-deref | memsafety | no-fp-cast | no-uncaught
//                          # concurrency/: norace | noassert | nodeadlock
//   expected:              # function -> true (no UB for any input) | false
//     add_false: false
//   witness:               # false functions: inputs that trigger the UB
//     add_false: [2147483647, 1]
//   timeout: 900           # optional: seconds per PRISM run for this file
//                          # (raises, never lowers, --timeout)
//   expect_status:         # optional: the exact status the laws demand
//     f: NEEDS-HARNESS     # (pointer parameters, Law 6)
//
// SV-COMP task definitions (format 2.0 .yml with `properties:`) are read as-is;
// the `no-overflow` / `valid-memsafety` verdict applies to `main`.
//
// Concurrency tasks (tests/conformance/concurrency, roadmap 2.6) are whole
// programs that create threads; `expected: {main: ...}` is scored by the conc
// stage only. conc never proves (a context-switch bound is not a proof), so
// its soundness line is zero wrong proofs by construction; what it measures is
// detection (FAILED in the task's property class) and false alarms. Its
// counterexamples are schedules, not inputs, so they are "refuted, not
// replayed".
//
// `--self-check` validates the suite's own labels without PRISM: every
// witness must trip a sanitizer, and every `true` function must survive an
// edge-value grid plus random inputs under the sanitizers.
//
// This executes task code: only the trusted suite in this repository (or the
// pinned Juliet archive, whose hash is checked) is ever compiled and run.

#include "support/cli.hpp"
#include "support/engine.hpp"
#include "support/fetch.hpp"
#include "support/pool.hpp"
#include "support/proctree.hpp"
#include "support/replay.hpp"
#include "support/report.hpp"
#include "support/task.hpp"
#include "support/yaml_subset.hpp"

#include "prism/regex.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

#include <unistd.h>

namespace prism::qa {

namespace {

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << text;
}

std::string dump(const ojson& j) { return j.dump(1, ' ', false, ojson::error_handler_t::replace); }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t a = 0;
    for (;;) {
        std::size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

ojson found_rec(const std::string& status, const std::string& message) {
    ojson rec = ojson::object();
    rec["status"] = status;
    rec["cls"] = "";
    rec["message"] = message;
    rec["counterexample"] = "";
    rec["line"] = nullptr;
    return rec;
}

fs::path make_work_dir(const std::string& prefix) {
    std::string tmpl = (fs::temp_directory_path() / (prefix + "XXXXXX")).string();
    if (!::mkdtemp(tmpl.data())) throw std::runtime_error("mkdtemp failed");
    return tmpl;
}

struct WorkDir {
    fs::path path;
    ~WorkDir() {
        if (std::getenv("PRISM_CONF_KEEP")) {
            std::cerr << "work dir kept: " << path.string() << "\n";
        } else {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};

}  // namespace

std::pair<std::vector<ojson>, int> self_check(const std::vector<Task>& tasks, const fs::path& work, int jobs) {
    struct Item {
        const Task* t;
        std::string fn;
        bool exp;
    };
    std::vector<Item> items;
    for (const auto& t : tasks) {
        if (t.origin != "prism" && t.origin != "esbmc-cpp") continue;
        for (const auto& [fn, exp] : t.expected) items.push_back({&t, fn, exp});
    }
    auto one = [&](const Item& it) -> ojson {
        const Task& t = *it.t;
        ojson rec = ojson::object();
        rec["task"] = t.ident;
        rec["function"] = it.fn;
        rec["expected"] = it.exp;
        if (t.origin == "esbmc-cpp") {
            // whole program: a deterministic one has one behaviour, so one
            // native run under the sanitizers (assertions on) decides it
            if (!t.deterministic) {
                rec["check"] = "skipped";
                rec["why"] = "nondeterministic program (label is ESBMC's)";
                return rec;
            }
            Exec res = run_program(t, work / "selfcheck" / path_key(t.ident));
            bool ok = res.outcome == (it.exp ? "clean" : "ub");
            rec["check"] = ok ? "ok" : "FAIL";
            rec["outcome"] = res.outcome;
            rec["detail"] = res.detail;
            return rec;
        }
        if (t.expect_status.count(it.fn)) {
            rec["check"] = "skipped";
            rec["why"] = "non-scalar parameters (expect_status)";
            return rec;
        }
        auto params = scalar_params(t, it.fn);
        if (!params) {
            rec["check"] = "skipped";
            rec["why"] = "no scalar signature";
            return rec;
        }
        fs::path wd = work / "selfcheck" / path_key(t.ident) / it.fn;
        Exec res;
        bool ok = false;
        if (it.exp) {
            res = run_sanitized(t, it.fn, *params, input_grid(*params), wd);
            ok = res.outcome == "clean";
        } else {
            auto w = t.witness.find(it.fn);
            if (w == t.witness.end()) {
                rec["check"] = "FAIL";
                rec["why"] = "false task without witness";
                return rec;
            }
            res = run_sanitized(t, it.fn, *params, {w->second}, wd);
            const bool blind = t.sanitizer_blind.count(it.fn) > 0;
            ok = res.outcome == "ub" || (blind && res.outcome == "clean");
            if (blind) rec["sanitizer_blind"] = true;
        }
        rec["check"] = ok ? "ok" : "FAIL";
        rec["outcome"] = res.outcome;
        rec["detail"] = res.detail;
        return rec;
    };
    auto out = parallel_map(jobs, items, one);
    int bad = 0;
    for (const auto& r : out)
        if (r["check"] == "FAIL") ++bad;
    return {out, bad};
}

namespace {

struct TaskResult {
    PrismRun run;
    std::optional<double> seconds_certified;
    std::string certified_error;
    bool has_cert = false;
};

bool parse_int(const std::string& s, long& v) {
    try {
        std::size_t k = 0;
        v = std::stol(s, &k);
        return k == s.size();
    } catch (...) {
        return false;
    }
}

bool parse_float(const std::string& s, double& v) {
    try {
        std::size_t k = 0;
        v = std::stod(s, &k);
        return k == s.size();
    } catch (...) {
        return false;
    }
}

}  // namespace

int conformance_main(const std::vector<std::string>& argv) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const std::vector<ArgSpec> specs = {
        {"--prism", "", true, false, "PRISM binary (default: $PRISM_BIN, else build/prism)"},
        {"--suite", "", true, true, "task roots (default: tests/conformance/{prism,sv-comp,concurrency,esbmc-cpp,libc-models})"},
        {"--juliet", "", true, false, "include Juliet tasks from DIR/juliet (see --fetch-juliet)"},
        {"--fetch-juliet", "", true, false,
         "download NIST Juliet 1.3, check sha256, extract CWE190/191/369/476/680 into DIR"},
        {"--juliet-flows", "", true, false, "Juliet flow variants to keep (comma list, default 01)"},
        {"--esbmc", "", true, false, "include ESBMC C++ regression tasks from DIR/esbmc-cpp"},
        {"--fetch-esbmc", "", true, false,
         "clone ESBMC's C++ regression tests at " + ESBMC_COMMIT.substr(0, 12) + " and convert them into DIR"},
        {"--esbmc-thorough", "", false, false, "also convert ESBMC's THOROUGH tests"},
        {"--curate-esbmc", "", true, false,
         "write the committed subset (tests/conformance/esbmc-cpp) from fetched tasks in DIR"},
        {"--stages", "", true, false, "override verdict stages (default: bmc,harness + pir,conc when listed)"},
        {"--out", "", true, false, "output directory (default: conformance-out)"},
        {"--jobs", "-j", true, false, "parallel PRISM runs (default: CPU count)"},
        {"--mem-limit-mb", "", true, false,
         "address-space cap per PRISM run in MB, inherited by its children (0: none); a run over it is scored as "
         "no answer, never as a proof (default 4096)"},
        {"--timeout", "", true, false, "seconds per PRISM run (default 180)"},
        {"--unwind", "", true, false, "--unwind for every PRISM run"},
        {"--filter", "", true, false, "regex on task ident"},
        {"--no-replay", "", false, false,
         "do not compile/run counterexamples (detection is then reported as 0 replayed)"},
        {"--self-check", "", false, false, "validate the suite labels with sanitizers instead of running PRISM"},
        {"--certified", "", false, false, "also run the pir stage with --certified and score it as 'pir-certified'"},
        {"--solver-cache", "", true, false,
         "solver query cache for the pir runs (default: a fresh one in the work dir, so no answer comes from an "
         "earlier run)"},
    };
    Args args;
    int rc = 0;
    if (!parse_args("prism-qa conformance", "PRISM conformance suite runner (roadmap 2.7) and release gate (roadmap 6.1).",
                    specs, argv, args, rc))
        return rc;
    long jobs = hw, mem_limit = 4096, unwind = 0;
    double timeout = 180.0;
    if ((args.has("--jobs") && !parse_int(args.get("--jobs"), jobs)) ||
        (args.has("--mem-limit-mb") && !parse_int(args.get("--mem-limit-mb"), mem_limit)) ||
        (args.has("--unwind") && !parse_int(args.get("--unwind"), unwind)) ||
        (args.has("--timeout") && !parse_float(args.get("--timeout"), timeout))) {
        std::cerr << "prism-qa conformance: error: invalid number\n";
        return 2;
    }
    jobs = std::max(1L, jobs);
    mem_limit = std::max(0L, mem_limit);

    try {
        if (args.has("--fetch-juliet")) {
            fetch_juliet(args.get("--fetch-juliet"), split(args.get("--juliet-flows", "01"), ','));
            return 0;
        }
        if (args.has("--fetch-esbmc")) {
            fetch_esbmc(args.get("--fetch-esbmc"), args.flags.count("--esbmc-thorough") > 0);
            return 0;
        }
        if (args.has("--curate-esbmc")) {
            fs::path d = args.get("--curate-esbmc");
            fs::path src = fs::exists(d / "esbmc-cpp") ? d / "esbmc-cpp" : d;
            WorkDir wd{make_work_dir("prism-curate-")};
            int n = curate_esbmc(src, suite_root() / "esbmc-cpp", 32, 3, wd.path);
            return n ? 0 : 1;
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    std::vector<Task> tasks;
    try {
        std::vector<fs::path> roots;
        if (args.values.count("--suite")) {
            for (const auto& s : args.values.at("--suite")) roots.emplace_back(s);
        } else {
            for (const auto& d : DEFAULT_ROOTS) roots.push_back(suite_root() / d);
        }
        std::vector<fs::path> existing;
        std::error_code ec;
        for (const auto& r : roots)
            if (fs::exists(r, ec)) existing.push_back(fs::weakly_canonical(fs::absolute(r), ec));
        tasks = discover(existing);
        if (args.has("--juliet")) {
            fs::path j = args.get("--juliet");
            fs::path jr = fs::exists(j / "juliet") ? j / "juliet" : j;
            for (const auto& yml : rglob_yml(jr)) {
                auto t = load_task(yml, jr.parent_path());
                if (t) {
                    t->origin = "juliet";
                    tasks.push_back(std::move(*t));
                }
            }
        }
        if (args.has("--esbmc")) {
            fs::path e = args.get("--esbmc");
            fs::path er = fs::exists(e / "esbmc-cpp") ? e / "esbmc-cpp" : e;
            for (const auto& yml : rglob_yml(er)) {
                auto t = load_task(yml, er.parent_path());
                if (t) {
                    t->origin = "esbmc-cpp";
                    t->ident = "esbmc-fetched/" + yml.lexically_relative(er).generic_string();
                    tasks.push_back(std::move(*t));
                }
            }
        }
    } catch (const yaml::Error& e) {
        std::cerr << "cannot load conformance tasks: " << e.what() << "\n";
        return 2;
    }
    if (args.has("--filter")) {
        Regex rx(args.get("--filter"));
        if (!rx.valid()) {
            std::cerr << "prism-qa conformance: error: bad --filter regex\n";
            return 2;
        }
        std::vector<Task> keep;
        for (auto& t : tasks)
            if (rx.search(t.ident)) keep.push_back(std::move(t));
        tasks = std::move(keep);
    }
    const fs::path out_dir = args.get("--out", "conformance-out");
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    WorkDir work{make_work_dir("prism-conf-")};

    if (args.flags.count("--self-check")) {
        auto [recs, bad] = self_check(tasks, work.path, static_cast<int>(jobs));
        write_file(out_dir / "selfcheck.json", dump(ojson(recs)));
        std::vector<std::pair<std::string, int>> counts;
        for (const auto& r : recs) {
            std::string c = r["check"].get<std::string>();
            auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& kv) { return kv.first == c; });
            if (it == counts.end()) counts.emplace_back(c, 1);
            else it->second++;
        }
        std::string cs = "{";
        for (std::size_t i = 0; i < counts.size(); ++i)
            cs += (i ? ", '" : "'") + counts[i].first + "': " + std::to_string(counts[i].second);
        cs += "}";
        std::cout << "self-check: " << cs << "\n";
        for (const auto& r : recs) {
            if (r["check"] != "FAIL") continue;
            std::cout << "  FAIL " << r["task"].get<std::string>() << " " << r["function"].get<std::string>()
                      << " expected=" << (r["expected"].get<bool>() ? "True" : "False") << " "
                      << r.value("outcome", "") << " " << r.value("why", "") << " "
                      << py_head(r.value("detail", ""), 200) << "\n";
        }
        return bad ? 1 : 0;
    }

    const std::vector<std::string> cmd = prism_command(args.get("--prism"));
    const std::vector<std::string> listed = list_stages(cmd);
    if (listed.empty()) {
        std::string c;
        for (const auto& x : cmd) c += (c.empty() ? "" : " ") + x;
        std::cerr << "cannot run PRISM: " << (c.empty() ? "(no --prism, $PRISM_BIN or build/prism)" : c)
                  << " --list-stages gave nothing\n";
        return 3;
    }
    auto is_listed = [&](const std::string& s) { return std::find(listed.begin(), listed.end(), s) != listed.end(); };
    std::vector<std::string> vstages;
    if (args.has("--stages")) {
        for (const auto& s : split(args.get("--stages"), ','))
            if (!s.empty()) vstages.push_back(s);
    } else {
        for (const auto& s : VERDICT_STAGES)
            if (is_listed(s)) vstages.push_back(s);
    }
    std::vector<std::string> run_stages;
    for (const auto& s : BASE_STAGES)
        if (is_listed(s)) run_stages.push_back(s);
    for (const auto& s : vstages)
        if (std::find(run_stages.begin(), run_stages.end(), s) == run_stages.end()) run_stages.push_back(s);
    // keep the pipeline's own order
    auto pos = [&](const std::string& s) {
        auto it = std::find(listed.begin(), listed.end(), s);
        return it == listed.end() ? std::size_t{999} : static_cast<std::size_t>(it - listed.begin());
    };
    std::stable_sort(run_stages.begin(), run_stages.end(),
                     [&](const std::string& a, const std::string& b) { return pos(a) < pos(b); });

    const fs::path cache = args.has("--solver-cache") ? fs::path(args.get("--solver-cache")) : work.path / "solver-cache";
    const std::vector<std::string> extra = {"--solver-cache", cache.string()};
    const bool cert_on = args.flags.count("--certified") && is_listed("pir");
    if (cert_on) vstages.push_back(CERT_STAGE);
    std::vector<std::string> cert_stages;
    for (const auto& s : {"inventory", "classify", "pir"})
        if (is_listed(s)) cert_stages.push_back(s);

    auto one = [&](const Task& t) -> TaskResult {
        TaskResult tr;
        tr.run = run_prism(cmd, t, run_stages, work.path, timeout, static_cast<int>(unwind), extra, {}, "prism",
                           mem_limit);
        if (cert_on && !tr.run.error) {
            // Certificates cost time (bit-blast, LRAT, checking): twice the budget.
            auto cx = extra;
            cx.push_back("--certified");
            PrismRun cres = run_prism(cmd, t, cert_stages, work.path, 2 * timeout, static_cast<int>(unwind), cx,
                                      {{"pir", CERT_STAGE}}, "prism-cert", mem_limit);
            if (cres.error) {
                auto& m = tr.run.findings[CERT_STAGE];
                m.clear();
                for (const auto& [fn, _] : t.expected) m[fn] = ojson::array({found_rec("STAGE-FAILED", py_head(*cres.error, 300))});
            } else {
                for (auto& [stage, fns] : cres.findings) tr.run.findings[stage] = fns;
            }
            tr.has_cert = true;
            tr.seconds_certified = cres.seconds;
            tr.certified_error = cres.error.value_or("");
        }
        return tr;
    };
    std::vector<TaskResult> results = parallel_map(static_cast<int>(jobs), tasks, one);

    std::vector<ojson> rows;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const Task& t = tasks[i];
        const TaskResult& res = results[i];
        const bool err = res.run.error.has_value();
        for (const auto& stage : vstages) {
            if (auto so = STAGE_TASK_ORIGINS.find(stage); so != STAGE_TASK_ORIGINS.end() && !so->second.count(t.origin))
                continue;
            if (auto os = ORIGIN_STAGES.find(t.origin); os != ORIGIN_STAGES.end() && !os->second.count(stage)) continue;
            for (const auto& [fn, exp] : t.expected) {
                ojson found = err ? ojson::array() : res.run.found(stage, fn);
                if (found.empty() && SCOPED_STAGES.count(stage) && !err)
                    continue;  // outside the stage's scope (e.g. harness: pointer functions)
                ojson row = ojson::object();
                row["task"] = t.ident;
                row["origin"] = t.origin;
                row["category"] = t.category;
                row["function"] = fn;
                row["expected"] = exp;
                row["property"] = t.prop;
                row["stage"] = stage;
                row["law_task"] = t.expect_status.count(fn) > 0;
                row["findings"] = found;
                if (stage == CERT_STAGE) row["seconds"] = res.seconds_certified ? ojson(*res.seconds_certified) : ojson();
                else row["seconds"] = res.run.seconds;
                if (err) row["error"] = *res.run.error;
                row["outcome"] = classify(t, fn, found);
                rows.push_back(std::move(row));
            }
        }
    }

    // counterexample replay for refutations of in-house tasks
    std::vector<std::size_t> to_replay;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i]["outcome"] == "refuted") to_replay.push_back(i);
    if (args.flags.count("--no-replay")) {
        for (auto i : to_replay) {
            ojson rp = ojson::object();
            rp["replay"] = "skipped";
            rp["why"] = "--no-replay";
            rows[i]["replay"] = rp;
        }
    } else {
        auto rep = [&](std::size_t i) -> ojson {
            const ojson& row = rows[i];
            const Task* t = nullptr;
            for (const auto& x : tasks)
                if (x.ident == row["task"].get<std::string>()) {
                    t = &x;
                    break;
                }
            std::string cex;
            for (const auto& f : row["findings"])
                if (f["status"] == "FAILED") {
                    cex = f.contains("counterexample") && f["counterexample"].is_string()
                              ? f["counterexample"].get<std::string>()
                              : "";
                    break;
                }
            return replay(*t, row["function"].get<std::string>(), cex, work.path / row["stage"].get<std::string>());
        };
        auto reps = parallel_map(static_cast<int>(jobs), to_replay, rep);
        for (std::size_t k = 0; k < to_replay.size(); ++k) rows[to_replay[k]]["replay"] = reps[k];
    }

    ojson metrics = compute_metrics(rows, vstages);
    long wrong = 0;
    for (const auto& r : rows)
        if (r["outcome"] == "wrong-proof") ++wrong;
    std::set<std::pair<std::string, std::string>> fnset;
    std::set<std::string> errors;
    for (const auto& r : rows) {
        fnset.insert({r["task"].get<std::string>(), r["function"].get<std::string>()});
        if (r.contains("error")) errors.insert(r["task"].get<std::string>() + ": " + r["error"].get<std::string>());
    }
    ojson meta = ojson::object();
    meta["engine"] = "cpp";
    meta["command"] = cmd;
    meta["stages"] = run_stages;
    meta["verdict_stages"] = vstages;
    meta["tasks"] = static_cast<long>(tasks.size());
    meta["functions"] = static_cast<long>(fnset.size());
    meta["sandbox"] = bwrap_ok() ? "bwrap" : "none (bwrap unavailable)";
    meta["wrong_proofs"] = wrong;
    meta["release_gate"] = wrong == 0 ? "PASS" : "FAIL";
    meta["errors"] = std::vector<std::string>(errors.begin(), errors.end());
    if (cert_on) {
        // Wrong proofs above already include the pir-certified rows.
        ojson cs = certified_summary(rows);
        // Cost of the certified runs, once per task (not per function).
        double secs = 0;
        long runs = 0;
        std::vector<std::string> timeouts;
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            if (!results[i].has_cert) continue;
            ++runs;
            secs += results[i].seconds_certified.value_or(0);
            if (results[i].certified_error.rfind("TIMEOUT", 0) == 0) timeouts.push_back(tasks[i].ident);
        }
        std::sort(timeouts.begin(), timeouts.end());
        cs["run_seconds"] = py_round(secs, 1);
        cs["runs"] = runs;
        cs["timeouts"] = timeouts;
        meta["certified"] = cs;
    }
    write_file(out_dir / "results.json", dump(ojson(rows)));
    ojson mj = ojson::object();
    mj["meta"] = meta;
    mj["metrics"] = metrics;
    write_file(out_dir / "metrics.json", dump(mj));
    std::string md = markdown(metrics, rows, meta);
    write_file(out_dir / "metrics.md", md);
    std::cout << md << "\n";
    if (wrong) {
        std::cerr << "RELEASE GATE FAILED: " << wrong << " wrong proof(s)\n";
        return 1;
    }
    return 0;
}

}  // namespace prism::qa
