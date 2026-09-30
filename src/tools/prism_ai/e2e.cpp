// End-to-end A/B of the learned scheduler on held-out VCs (roadmap 3.1 / 9.7).
//
// The replay in sched.cpp assumes no contention and no process start-up; this
// runs the real portfolio instead. For every held-out source file (the split
// of sched.cpp) the pir VCs are written with `prism --pir-vcs` (up to 6 per
// function, unwind 8) and each VC is answered by `prism --solve-smt2` four
// times, interleaved so that a change in machine load hits every pass alike:
//
//   rules    PRISM_SOLVER_PREDICT=0, a fresh history per VC (no history)
//   model    PRISM_SOLVER_MODEL=<GBDT trained on the training files, enabled>
//   history  PRISM_SOLVER_PREDICT=0, one history across the run
//   rules2   rules again: the run-to-run noise
//
// Fresh processes with per-pass environments are the point of the
// measurement. Solver wall time (process start excluded) is summed; a timeout
// or a stall (no answer within 120 s: the process group is killed) counts as
// the timeout. Answers are compared across passes. Output: <out>/e2e.json;
// `prism_ai predict --end-to-end` adds its summary to the model's metrics.
//
//     prism_ai e2e --prism build/prism --solve-log DIR/solve_runs.jsonl --out e2e

#include "prism_ai.hpp"
#include "pycompat.hpp"

#include "prism/solver_predict.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef PRISM_REPO_DIR
#define PRISM_REPO_DIR "."
#endif

namespace prism_ai::e2e {

ProcResult run_with_env(const std::vector<std::string>& argv, const std::vector<std::pair<std::string, std::string>>& set,
                        const std::vector<std::string>& unset, double timeout_s) {
    ProcResult r;
#ifdef _WIN32
    (void)argv, (void)set, (void)unset, (void)timeout_s;
    r.rc = 127;
    r.out = "";
    return r;
#else
    if (argv.empty()) return r;
    int fds[2];
    if (::pipe(fds) != 0) return r;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return r;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(fds[1], 1);
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            ::dup2(devnull, 2);
        }
        ::close(fds[0]);
        ::close(fds[1]);
        for (const auto& u : unset) ::unsetenv(u.c_str());
        for (const auto& [k, v] : set) ::setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    ::close(fds[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
    char buf[65536];
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            r.timed_out = true;
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int pr = ::poll(&p, 1, static_cast<int>(std::min<long long>(left.count(), 1000)));
        if (pr < 0) break;
        if (pr == 0) continue;
        const auto n = ::read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        r.out.append(buf, static_cast<std::size_t>(n));
    }
    if (r.timed_out) ::kill(-pid, SIGKILL);
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    r.rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
#endif
}

int e2e_main(int argc, char** argv, std::ostream& out, std::ostream& err) {
    std::string prism;
    std::vector<fs::path> logs;
    std::optional<fs::path> odir;
    double timeout = 8.0;
    int trees = 60;
    fs::path repo = PRISM_REPO_DIR;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--prism" && has) prism = argv[++i];
        else if (a == "--solve-log" && has) logs.emplace_back(argv[++i]);
        else if (a == "--out" && has) odir = argv[++i];
        else if (a == "--timeout" && has) timeout = std::stod(argv[++i]);
        else if (a == "--trees" && has) trees = std::stoi(argv[++i]);
        else if (a == "--repo" && has) repo = argv[++i];
        else {
            err << "usage: prism_ai e2e --prism BIN --solve-log FILE... --out DIR [--timeout 8] [--trees 60] [--repo DIR]\n";
            return 2;
        }
    }
    if (prism.empty() || logs.empty() || !odir) {
        err << "prism_ai e2e: --prism, --solve-log and --out are required\n";
        return 2;
    }
#ifdef _WIN32
    err << "prism_ai e2e: NOTRUN on Windows (needs process groups and per-process environments)\n";
    return 3;
#endif
    fs::create_directories(*odir);
    auto [train, test] = sched::split(sched::load(logs));
    sched::GbdtModel g(train, false, trees);
    ojson model = ojson::object();
    model["schema"] = 1;
    model["kind"] = "prism-gbdt";
    model["enabled"] = true;
    model["query_features"] = prism::solver::predict::query_feature_names();
    model["function_features"] = prism::solver::predict::function_feature_names();
    model["solvers"] = ojson::object();
    for (const auto& [m, x] : g.models) model["solvers"][m] = x.to_json();
    const auto mp = *odir / "model.json";
    std::ofstream(mp, std::ios::binary) << model.dump();
    std::set<std::string> fset;
    for (const auto& r : test) fset.insert(r.key);
    const std::vector<std::string> files(fset.begin(), fset.end());
    std::vector<std::string> vcs;
    for (const auto& f : files) {
        std::string d = (*odir / "vcs").string() + "/";
        for (char c : f) d += c == '/' ? std::string("__") : std::string(1, c);
        auto r = run_with_env({prism, "--pir-vcs", (repo / f).string(), "--out", d, "--unwind", "8"}, {}, {}, 600.0);
        try {
            const auto j = ojson::parse(r.out);
            if (j.contains("functions"))
                for (const auto& fn : j["functions"]) {
                    if (!fn.contains("vcs")) continue;
                    std::size_t k = 0;
                    for (const auto& vc : fn["vcs"]) {
                        if (k++ >= 6) break;
                        vcs.push_back(py(vc.at("path")));
                    }
                }
        } catch (const std::exception&) {
            continue;
        }
    }
    const std::vector<std::string> passes = {"rules", "model", "history", "rules2"};
    std::map<std::string, std::vector<ojson>> res;
    for (std::size_t i = 0; i < vcs.size(); ++i)
        for (const auto& p : passes) {
            std::vector<std::pair<std::string, std::string>> set;
            if (p == "model") set.emplace_back("PRISM_SOLVER_MODEL", mp.string());
            else set.emplace_back("PRISM_SOLVER_PREDICT", "0");
            const auto hist = p == "history" ? *odir / "hist-shared" : *odir / "hist" / p / std::to_string(i);
            auto r = run_with_env({prism, "--solve-smt2", vcs[i], "--timeout", py_repr(timeout), "--solver-cache",
                                   hist.string()},
                                  set, {"PRISM_SOLVER_MODEL"}, 120.0);
            ojson j;
            if (r.timed_out) j = ojson{{"kind", "stall"}};
            else {
                try {
                    j = ojson::parse(r.out);
                } catch (const std::exception&) {
                    j = ojson{{"kind", "error"}};
                }
            }
            res[p].push_back(ojson{{"kind", j.contains("kind") ? j["kind"] : ojson(nullptr)},
                                   {"wall_s", j.contains("wall_s") ? j["wall_s"] : ojson(timeout)},
                                   {"winner", j.contains("winner") ? j["winner"] : ojson("")}});
        }
    ojson summary = ojson::object();
    auto answered = [](const ojson& x) {
        return x["kind"].is_string() && (x["kind"] == "sat" || x["kind"] == "unsat");
    };
    for (const auto& p : passes) {
        std::vector<double> walls;
        int timeouts = 0, stalls = 0;
        ojson wins = ojson::object();
        for (const auto& x : res[p]) {
            const bool ok = answered(x);
            walls.push_back(ok && x["wall_s"].is_number() ? x["wall_s"].get<double>() : timeout);
            timeouts += !ok;
            stalls += x["kind"] == "stall";
            const auto w = py(x["winner"]);
            wins[w] = wins.value(w, 0) + 1;
        }
        double total = 0.0;
        for (double w : walls) total += w;
        auto s = walls;
        std::sort(s.begin(), s.end());
        summary[p] = ojson{{"total_s", py_round(total, 3)},
                           {"timeouts", timeouts},
                           {"stalls", stalls},
                           {"median_s", s.empty() ? 0.0 : py_round(s[s.size() / 2], 4)},
                           {"winners", wins}};
    }
    int disagreements = 0;
    for (std::size_t i = 0; i < vcs.size(); ++i) {
        std::set<std::string> kinds;
        for (const auto& p : passes)
            if (answered(res[p][i])) kinds.insert(res[p][i]["kind"].get<std::string>());
        disagreements += kinds.size() > 1;
    }
    summary["disagreements"] = disagreements;
    summary["vcs"] = vcs.size();
    summary["files"] = files.size();
    ojson resj = ojson::object();
    for (const auto& p : passes) resj[p] = res[p];
    std::ofstream(*odir / "e2e.json", std::ios::binary)
        << ojson{{"summary", summary}, {"res", resj}, {"vcs", vcs}}.dump(1);
    out << summary.dump(1) << "\n";
    return 0;
}

}  // namespace prism_ai::e2e
