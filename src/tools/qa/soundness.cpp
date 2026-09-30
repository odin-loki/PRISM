// prism-qa soundness: random-program soundness testing for PRISM (roadmap
// 5.5 / 6.2).
//
// Generate small C programs, run PRISM's verdict stages on them, and for every
// function PRISM *proves* (PROVED / PROVED-UNBOUNDED / PROVED-ASSUMING /
// PROVED-CERTIFIED) execute it concretely under UBSan+ASan on an edge-value
// grid plus random inputs. Any sanitizer report on a proved function is a
// soundness bug: the repro (program, function, inputs, sanitizer message) is
// written to OUT/bugs/ and printed, and the exit status is 1.
//
// Refutations are checked too: a FAILED counterexample is replayed, and a
// FAILED function on which neither the counterexample nor the input grid
// triggers the sanitizer is listed as a suspected false alarm.
//
// Generators (src/tools/qa/gen_random.cpp):
//   inhouse       (default) scalar expressions, guards, bounded loops
//   inhouse-ptr   pointer programs for the pir memory model
//   inhouse-loop  symbolic-bound loops for the pir loop invariants
//   csmith        Csmith (external; NOTRUN, exit 3, when not on PATH) in a
//                 scalar-only configuration (no pointers/structs/arrays/
//                 globals, --no-safe-math so UB can occur). Only functions
//                 with scalar parameters are driven.
//
// Programs come from a Python-compatible Mersenne Twister (support/pyrandom.hpp)
// drawn in the original order, so a --seed reproduces the programs it named
// before this tool was C++.
//
// Only programs this tool generates are compiled and run (inside bwrap when
// available), never code from elsewhere.

#include "gen_random.hpp"
#include "support/cli.hpp"
#include "support/engine.hpp"
#include "support/pool.hpp"
#include "support/proctree.hpp"
#include "support/replay.hpp"
#include "support/task.hpp"

#include "prism/regex.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

#include <unistd.h>

namespace prism::qa {

namespace {

const std::vector<std::string> CSMITH_FLAGS = {
    "--no-pointers", "--no-structs", "--no-unions", "--no-arrays", "--no-safe-math", "--no-volatiles",
    "--no-bitfields", "--no-global-variables", "--no-checksum", "--nomain", "--max-funcs", "3", "--no-argc",
    "--no-jumps", "--no-float", "--no-packed-struct", "--max-block-depth", "3", "--max-expr-complexity", "5",
    "--no-inline-function", "--concise"};
const char* const CSMITH_H =
    "#ifndef PRISM_CSMITH_STUB_H\n#define PRISM_CSMITH_STUB_H\n"
    "/* Minimal stand-in for csmith.h: scalar-only programs need only stdint. */\n"
    "#include <stdint.h>\n#include <stdio.h>\n#endif\n";

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << text;
}

std::string gen_csmith(long long seed, const fs::path& work) {
    std::string exe = which("csmith");
    if (exe.empty()) return "";
    fs::path out = work / ("cs" + std::to_string(seed) + ".c");
    std::vector<std::string> argv = {exe, "--seed", std::to_string(seed)};
    argv.insert(argv.end(), CSMITH_FLAGS.begin(), CSMITH_FLAGS.end());
    argv.insert(argv.end(), {"-o", out.string()});
    RunOpts o;
    o.timeout_s = 120;
    o.cwd = work;  // csmith drops platform.info into its cwd: keep it in the work dir
    auto r = run_tree(argv, o);
    std::error_code ec;
    if (r.start_failed || r.timed_out || r.rc != 0 || !fs::exists(out, ec)) return "";
    std::string text = utf8_clean(read_text(out));
    // csmith's helpers are `static`; make them visible to the driver's #include (same TU anyway).
    const std::string inc = "#include \"csmith.h\"";
    for (std::size_t p; (p = text.find(inc)) != std::string::npos;) text.replace(p, inc.size(), CSMITH_H);
    return text;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

Task program_task(const fs::path& program, const std::vector<std::string>& fns, const std::string& origin) {
    Task t;
    t.ident = program.filename().string();
    t.yml = program;
    t.source = program;
    t.origin = origin;
    t.category = "random";
    t.lang = "C";
    for (const auto& f : fns) t.expected.emplace_back(f, true);
    return t;
}

struct Analysed {
    std::string program;
    std::vector<std::pair<std::string, ojson>> functions;
    std::optional<std::string> error;
};

std::set<std::string> statuses(const ojson& found) {
    std::set<std::string> s;
    for (const auto& f : found) s.insert(f.contains("status") && f["status"].is_string() ? f["status"].get<std::string>() : "None");
    return s;
}

bool any_proof(const std::set<std::string>& st) {
    for (const auto& p : PROOF)
        if (st.count(p)) return true;
    return false;
}

}  // namespace

int soundness_main(const std::vector<std::string>& argv) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const std::vector<ArgSpec> specs = {
        {"--programs", "-n", true, false, "number of programs (default 300)"},
        {"--funcs", "", true, false, "functions per in-house program (default 3)"},
        {"--seed", "", true, false, "seed of the first program (default 1)"},
        {"--generator", "", true, false, "inhouse | inhouse-ptr | inhouse-loop | csmith (default inhouse)"},
        {"--prism", "", true, false, "PRISM binary (default: $PRISM_BIN, else build/prism)"},
        {"--out", "", true, false, "output directory (default: soundness-out)"},
        {"--jobs", "-j", true, false, "parallel runs (default: CPU count)"},
        {"--timeout", "", true, false, "seconds per PRISM run (default 120)"},
    };
    Args args;
    int rc = 0;
    if (!parse_args("prism-qa soundness", "Random-program soundness testing for PRISM (roadmap 5.5 / 6.2).", specs, argv,
                    args, rc))
        return rc;
    long long programs_n = 300, funcs = 3, seed = 1, jobs = hw;
    double timeout = 120.0;
    try {
        if (args.has("--programs")) programs_n = std::stoll(args.get("--programs"));
        if (args.has("--funcs")) funcs = std::stoll(args.get("--funcs"));
        if (args.has("--seed")) seed = std::stoll(args.get("--seed"));
        if (args.has("--jobs")) jobs = std::stoll(args.get("--jobs"));
        if (args.has("--timeout")) timeout = std::stod(args.get("--timeout"));
    } catch (...) {
        std::cerr << "prism-qa soundness: error: invalid number\n";
        return 2;
    }
    jobs = std::max(1LL, jobs);
    const std::string generator = args.get("--generator", "inhouse");
    if (generator != "inhouse" && generator != "inhouse-ptr" && generator != "inhouse-loop" && generator != "csmith") {
        std::cerr << "prism-qa soundness: error: --generator: invalid choice '" << generator
                  << "' (choose from inhouse, inhouse-ptr, inhouse-loop, csmith)\n";
        return 2;
    }
    if (generator == "csmith" && which("csmith").empty()) {
        std::cerr << "csmith: NOTRUN (not on PATH; apt install csmith). Use --generator inhouse.\n";
        return 3;
    }
    const std::vector<std::string> cmd = prism_command(args.get("--prism"));
    const std::vector<std::string> listed = list_stages(cmd);
    if (listed.empty()) {
        std::cerr << "cannot run PRISM: " << (cmd.empty() ? "(no --prism, $PRISM_BIN or build/prism)" : join(cmd, " "))
                  << "\n";
        return 3;
    }
    std::vector<std::string> stages;
    for (const auto& s : {"inventory", "classify", "bmc", "pir"})
        if (std::find(listed.begin(), listed.end(), s) != listed.end()) stages.push_back(s);

    const fs::path out_dir = args.get("--out", "soundness-out");
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    const fs::path progs_dir = out_dir / "programs";
    fs::create_directories(progs_dir, ec);
    std::string tmpl = (fs::temp_directory_path() / "prism-rand-XXXXXX").string();
    if (!::mkdtemp(tmpl.data())) {
        std::cerr << "mkdtemp failed\n";
        return 2;
    }
    const fs::path work = tmpl;
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            if (std::getenv("PRISM_CONF_KEEP")) {
                std::cerr << "work dir kept: " << p.string() << "\n";
            } else {
                std::error_code e;
                fs::remove_all(p, e);
            }
        }
    } cleanup{work};

    std::vector<std::pair<fs::path, std::vector<std::string>>> programs;
    static const Regex inhouse_fn(R"((?m)^int (rf\d+_\d+)\()");
    static const Regex csmith_fn(R"((?m)^static \w+\s+(func_\d+)\()");
    for (long long k = 0; k < programs_n; ++k) {
        const long long s = seed + k;
        std::string text;
        std::vector<std::string> fns;
        if (generator == "csmith") {
            text = gen_csmith(s, work);
            for (const auto& m : csmith_fn.finditer(text)) fns.push_back(m.group(1));
            if (text.empty()) continue;
        } else {
            const int nf = static_cast<int>(funcs);
            text = generator == "inhouse"       ? gen_inhouse(s, nf)
                   : generator == "inhouse-ptr" ? gen_inhouse_ptr(s, nf)
                                                : gen_inhouse_loop(s, nf);
            for (const auto& m : inhouse_fn.finditer(text)) fns.push_back(m.group(1));
        }
        fs::path p = progs_dir / (generator + "_" + std::to_string(s) + ".c");
        write_file(p, text);
        std::set<std::string> uniq(fns.begin(), fns.end());
        programs.emplace_back(p, std::vector<std::string>(uniq.begin(), uniq.end()));
    }

    auto analyse = [&](const std::pair<fs::path, std::vector<std::string>>& pf) -> Analysed {
        Analysed a;
        a.program = pf.first.string();
        Task task = program_task(pf.first, pf.second, "random");
        PrismRun res = run_prism(cmd, task, stages, work, timeout, 0);
        if (res.error) {
            a.error = res.error;
            return a;
        }
        for (const auto& fn : pf.second) {
            ojson found = ojson::array();
            for (const auto& f : res.found("bmc", fn)) found.push_back(f);
            for (const auto& f : res.found("pir", fn)) found.push_back(f);
            a.functions.emplace_back(fn, found);
        }
        return a;
    };
    std::vector<Analysed> analysed = parallel_map(static_cast<int>(jobs), programs, analyse);

    struct Job {
        fs::path program;
        std::string fn;
        ojson found;
    };
    std::vector<Job> jobs_list;
    for (std::size_t i = 0; i < programs.size(); ++i)
        for (const auto& [fn, found] : analysed[i].functions) {
            auto st = statuses(found);
            if (any_proof(st) || st.count("FAILED")) jobs_list.push_back({programs[i].first, fn, found});
        }
    auto check = [&](const Job& j) -> ojson {
        Task task = program_task(j.program, {j.fn}, "prism");
        auto st = statuses(j.found);
        ojson out = ojson::object();
        out["program"] = j.program.string();
        out["function"] = j.fn;
        out["statuses"] = std::vector<std::string>(st.begin(), st.end());
        auto params = scalar_params(task, j.fn);
        if (!params) {
            out["exec"] = "unsupported";
            return out;
        }
        fs::path wd = work / "exec" / j.program.stem() / j.fn;
        auto grid = input_grid(*params, 3000, seed);
        Exec ex = run_sanitized(task, j.fn, *params, grid, wd / "grid", 60);
        ojson g = ojson::object();
        g["outcome"] = ex.outcome;
        g["detail"] = ex.detail;
        out["grid"] = g;
        if (st.count("FAILED")) {
            std::string cex;
            for (const auto& f : j.found)
                if (f.contains("status") && f["status"] == "FAILED") {
                    cex = f.contains("counterexample") && f["counterexample"].is_string()
                              ? f["counterexample"].get<std::string>()
                              : "";
                    break;
                }
            out["replay"] = replay(task, j.fn, cex, wd);
        }
        return out;
    };
    std::vector<ojson> checks = parallel_map(static_cast<int>(jobs), jobs_list, check);

    ojson tally = ojson::object();
    long closed_inv = 0;
    for (const auto& a : analysed) {
        for (const auto& [fn, found] : a.functions) {
            auto st = statuses(found);
            std::string key = join(std::vector<std::string>(st.begin(), st.end()), "/");
            if (key.empty()) key = "MISSING";
            tally[key] = tally.value(key, 0L) + 1;
            for (const auto& f : found)
                if (f.contains("extra") && f["extra"].is_object() && f["extra"].value("k_induction", ojson()) == "closed-invariants") {
                    ++closed_inv;
                    break;
                }
        }
        if (a.error) tally["RUN-ERROR"] = tally.value("RUN-ERROR", 0L) + 1;
    }
    std::vector<const ojson*> bugs, alarms;
    long proofs_checked = 0, fails_replayed = 0, fails = 0;
    for (const auto& c : checks) {
        std::set<std::string> st;
        for (const auto& s : c["statuses"]) st.insert(s.get<std::string>());
        if (c.contains("exec") && c["exec"] == "unsupported") continue;
        if (any_proof(st)) {
            ++proofs_checked;
            if (c["grid"]["outcome"] == "ub") bugs.push_back(&c);
        }
        if (st.count("FAILED")) {
            ++fails;
            if (c.contains("replay") && c["replay"].value("replay", "") == "replayed") ++fails_replayed;
            else if (c["grid"]["outcome"] != "ub") alarms.push_back(&c);
        }
    }
    long nfuncs = 0;
    for (const auto& p : programs) nfuncs += static_cast<long>(p.second.size());
    ojson summary = ojson::object();
    summary["engine"] = "cpp";
    summary["command"] = cmd;
    summary["stages"] = stages;
    summary["generator"] = generator;
    summary["programs"] = static_cast<long>(programs.size());
    summary["functions"] = nfuncs;
    summary["verdicts"] = tally;
    summary["closed_by_invariants"] = closed_inv;
    summary["proofs_executed"] = proofs_checked;
    summary["wrong_proofs"] = static_cast<long>(bugs.size());
    summary["failed"] = fails;
    summary["failed_cex_replayed"] = fails_replayed;
    summary["suspected_false_alarms"] = static_cast<long>(alarms.size());
    summary["sandbox"] = bwrap_ok() ? "bwrap" : "none";
    const fs::path bugdir = out_dir / "bugs";
    fs::create_directories(bugdir, ec);
    auto dump = [](const ojson& j) { return j.dump(1, ' ', false, ojson::error_handler_t::replace); };
    std::vector<std::string> lines = {"# PRISM random-program soundness campaign", "", "```", dump(summary), "```", ""};
    lines.push_back("## Wrong proofs (" + std::to_string(bugs.size()) + ")");
    for (const ojson* c : bugs) {
        fs::path p = (*c)["program"].get<std::string>();
        std::string fn = (*c)["function"].get<std::string>();
        fs::copy_file(p, bugdir / p.filename(), fs::copy_options::overwrite_existing, ec);
        std::vector<std::string> st;
        for (const auto& s : (*c)["statuses"]) st.push_back(s.get<std::string>());
        lines.push_back("- `" + p.filename().string() + "` `" + fn + "` " + join(st, "/") + ": " +
                        (*c)["grid"]["detail"].get<std::string>());
        std::string src = read_text(p);
        std::string pat = "(?ms)^int " + fn + R"(\(.*?^\})";
        if (auto m = Regex(pat).search_match(src)) lines.insert(lines.end(), {"", "```c", m->group(0), "```", ""});
    }
    lines.push_back("\n## Suspected false alarms (" + std::to_string(alarms.size()) + ")");
    for (const ojson* c : alarms) {
        const ojson rp = c->contains("replay") ? (*c)["replay"] : ojson::object();
        std::string inputs = "None";
        if (rp.contains("inputs")) {
            // Python dict repr: {'a': 1, 'b': -2}
            inputs = "{";
            bool first = true;
            for (const auto& [k, v] : rp["inputs"].items()) {
                inputs += (first ? "'" : ", '") + k + "': " + v.dump();
                first = false;
            }
            inputs += "}";
        }
        lines.push_back("- `" + fs::path((*c)["program"].get<std::string>()).filename().string() + "` `" +
                        (*c)["function"].get<std::string>() + "` FAILED; cex replay " +
                        (rp.contains("replay") ? rp["replay"].get<std::string>() : std::string("None")) +
                        " inputs=" + inputs + "; grid " + (*c)["grid"]["outcome"].get<std::string>());
    }
    ojson full = ojson::object();
    full["summary"] = summary;
    full["checks"] = checks;
    write_file(out_dir / "summary.json", dump(full));
    write_file(out_dir / "summary.md", join(lines, "\n") + "\n");
    std::cout << join(lines, "\n") << "\n";
    return bugs.empty() ? 0 : 1;
}

}  // namespace prism::qa
