// solver-bench: the portfolio against Z3 alone on the pir VCs of the
// conformance suite (the roadmap 3.1 exit criterion: "the portfolio beats Z3
// alone on total time over the conformance suite's VCs").
//
// 1. `prism --pir-vcs FILE --out DIR` writes every verification condition of
//    every encodable function of each conformance task as an SMT-LIB2 file
//    (the VCs the pir stage solves, docs/PIR.md; src/prism/pir/bench.cpp);
// 2. each VC is answered by `prism --solve-smt2 VC` three times, back to back
//    (a change in machine load then hits the three alike; with -j > 1 one
//    VC's passes still run inside one worker):
//      z3         Z3 alone in-process (--z3-only),
//      portfolio  the portfolio with no solve-time history (a fresh one per VC),
//      history    the portfolio scheduling from the history it has built on
//                 the VCs before this one in the same run (what pir does);
//    one process per solve, so process_s is measured and a timeout stays
//    isolated; the query cache is off in all three (timing, not answers);
// 3. the solver's own wall time (wall_s, process start-up excluded) is summed
//    per pass; a timeout counts as the full timeout. A VC where two passes
//    give different definitive answers is a disagreement (a solver bug) and
//    makes the exit status 1.
//
// Output: <out>/solver_bench.json and <out>/solver_bench.md.

#include "qa.hpp"

#include "../../prism/proc.hpp"
#include "prism/regex.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace prism::qa {

namespace {
const std::vector<std::string> PASSES = {"z3", "portfolio", "history"};

std::string unquote(std::string s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && ws(s.back())) s.pop_back();
    while (!s.empty() && ws(s.front())) s.erase(s.begin());
    if (auto h = s.find(" #"); h != std::string::npos) s = s.substr(0, h);
    while (!s.empty() && ws(s.back())) s.pop_back();
    while (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
    return s;
}

// The sidecar keys task discovery needs, read line by line: the suite's
// sidecars are flat block YAML (a top-level `input_files:` scalar or flow
// list, an `expected:` map, or a `properties:` block sequence).
std::optional<fs::path> task_source(const fs::path& yml) {
    static const std::map<std::string, std::string> SV = {{"no-overflow.prp", "no-overflow"},
                                                         {"valid-memsafety.prp", "memsafety"}};
    std::istringstream in(read_text(yml));
    std::string input;
    bool expected = false, scored_property = false, in_props = false;
    std::string prop_file;
    bool prop_verdict = false;
    auto close_item = [&] {
        if (!prop_file.empty() && prop_verdict && SV.contains(fs::path(prop_file).filename().string()))
            scored_property = true;
        prop_file.clear();
        prop_verdict = false;
    };
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const bool top = line.front() != ' ' && line.front() != '-';
        if (top) {
            if (in_props) close_item();
            in_props = false;
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            const auto key = line.substr(0, colon);
            auto val = unquote(line.substr(colon + 1));
            if (key == "input_files") {
                if (val.starts_with("[")) {
                    val = val.substr(1, val.find_first_of(",]") - 1);
                    val = unquote(val);
                }
                input = val;
            } else if (key == "expected") {
                expected = true;
            } else if (key == "properties") {
                in_props = true;
            }
            continue;
        }
        if (!in_props) continue;
        auto t = line.substr(line.find_first_not_of(' '));
        if (t.starts_with("- ")) {
            close_item();
            t = t.substr(2);
        }
        const auto colon = t.find(':');
        if (colon == std::string::npos) continue;
        const auto key = t.substr(0, colon);
        if (key == "property_file") prop_file = unquote(t.substr(colon + 1));
        else if (key == "expected_verdict") prop_verdict = true;
    }
    if (in_props) close_item();
    if (input.empty() || !(expected || scored_property)) return std::nullopt;
    return yml.parent_path() / input;
}

std::string pyfloat(double v) {
    auto s = std::format("{}", v);
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

double round_to(double v, int digits) {
    const double f = std::pow(10.0, digits);
    return std::round(v * f) / f;
}

std::string py_str(const nlohmann::json& j) {
    if (j.is_null()) return "None";
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number_float()) return pyfloat(j.get<double>());
    return j.dump();
}

nlohmann::json solve(const fs::path& prism, const std::string& vc, bool z3, double timeout, const fs::path& hist,
                     const fs::path& repo) {
    std::vector<std::string> argv{prism.string(), "--solve-smt2", vc, "--timeout", pyfloat(timeout),
                                  "--solver-cache", hist.string()};
    if (z3) argv.push_back("--z3-only");
    detail::SessionOptions so;
    so.cwd = repo;
    so.timeout_s = timeout * 3 + 30;
    auto r = detail::run_session(argv, so);
    nlohmann::json res;
    if (r.timed_out) {
        res = {{"kind", "error"}, {"note", "driver: TimeoutExpired"}};
    } else {
        try {
            res = nlohmann::json::parse(r.out);
            if (!res.is_object()) throw std::runtime_error("not an object");
        } catch (const std::exception&) {
            res = {{"kind", "error"}, {"note", "driver: JSONDecodeError"}};
        }
    }
    res["process_s"] = round_to(r.seconds, 4);
    return res;
}

double charged(const nlohmann::json& r, double timeout) {
    const auto k = r.value("kind", nlohmann::json());
    if (k == "sat" || k == "unsat") {
        auto w = r.value("wall_s", nlohmann::json(0.0));
        return w.is_number() ? w.get<double>() : 0.0;
    }
    return timeout;
}
}  // namespace

std::vector<BenchTask> discover_tasks(const std::vector<fs::path>& roots, const fs::path& suite) {
    std::vector<BenchTask> out;
    std::error_code ec;
    const auto suite_c = fs::weakly_canonical(suite, ec);
    for (const auto& root_in : roots) {
        const auto root = fs::weakly_canonical(root_in, ec);
        const auto rel_to_suite = fs::relative(root, suite_c, ec);
        const bool under = !ec && !rel_to_suite.empty() && !rel_to_suite.string().starts_with("..");
        const auto base = (root == suite_c || under) ? suite_c : root.parent_path();
        std::vector<fs::path> ymls;
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            if (it->is_regular_file() && it->path().extension() == ".yml") ymls.push_back(it->path());
        std::sort(ymls.begin(), ymls.end());
        for (const auto& y : ymls) {
            auto src = task_source(y);
            if (!src || !fs::exists(*src, ec)) continue;
            out.push_back({fs::relative(y, base, ec).generic_string(), *src});
        }
    }
    return out;
}

int solver_bench_main(const Args& args) {
    const auto repo = repo_root();
    const auto suite = repo / "tests" / "conformance";
    std::string bin, filter, passes_arg = "z3,portfolio,history";
    std::vector<fs::path> roots;
    int unwind = 8, jobs = 1;
    double timeout = 30.0;
    bool list_tasks = false;
    fs::path out = "solver-bench-out";
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--prism" || a == "--bin") bin = next();
        else if (a == "--suite") roots.emplace_back(next());
        else if (a == "--filter") filter = next();
        else if (a == "--unwind") unwind = std::atoi(next().c_str());
        else if (a == "--timeout") timeout = std::atof(next().c_str());
        else if (a == "--jobs" || a == "-j") jobs = std::max(1, std::atoi(next().c_str()));
        else if (a == "--out") out = next();
        else if (a == "--passes") passes_arg = next();
        else if (a == "--list-tasks") list_tasks = true;
        else if (a == "-h" || a == "--help") {
            std::cout << "prism-qa solver-bench [--prism PRISM] [--suite DIR]... [--filter RE] [--unwind 8]\n"
                         "                      [--timeout 30] [-j 1] [--out solver-bench-out]\n"
                         "                      [--passes z3,portfolio,history] [--list-tasks]\n"
                         "Portfolio vs Z3 alone on the pir VCs of the conformance suite (default suites:\n"
                         "tests/conformance/prism and tests/conformance/sv-comp). Writes\n"
                         "<out>/solver_bench.json and <out>/solver_bench.md; exit 1 on a disagreement.\n";
            return 0;
        } else {
            std::cerr << "solver-bench: unknown option " << a << "\n";
            return 2;
        }
    }
    if (roots.empty()) roots = {suite / "prism", suite / "sv-comp"};
    for (auto& r : roots) r = fs::absolute(r);
    auto tasks = discover_tasks(roots, suite);
    if (!filter.empty()) {
        Regex rx(filter);
        std::erase_if(tasks, [&](const BenchTask& t) { return !rx.search(t.ident); });
    }
    if (list_tasks) {
        for (const auto& t : tasks) std::cout << t.ident << " " << t.source.string() << "\n";
        return 0;
    }
    auto prism = find_prism(bin, repo);
    if (!prism) {
        std::cerr << "C++ prism binary not found: pass --prism or set PRISM_BIN\n";
        return 2;
    }
    std::error_code ec;
    fs::create_directories(out, ec);
    TempDir work("prism-solver-bench-");
    std::vector<nlohmann::json> vcs;
    std::vector<std::string> errors;
    int unencoded = 0;
    for (const auto& t : tasks) {
        std::string dir_name;
        for (char c : t.ident) dir_name += c == '/' ? std::string("__") : std::string(1, c);
        detail::SessionOptions so;
        so.cwd = repo;
        so.timeout_s = 600;
        auto r = detail::run_session({prism->string(), "--pir-vcs", t.source.string(), "--out",
                                      (work.path / "vcs" / dir_name).string(), "--unwind", std::to_string(unwind)},
                                     so);
        nlohmann::json d;
        try {
            d = nlohmann::json::parse(r.out);
        } catch (const std::exception&) {
            const auto& e = r.err.empty() ? r.out : r.err;
            d = {{"file", t.source.string()}, {"error", e.substr(e.size() > 400 ? e.size() - 400 : 0)}};
        }
        if (d.contains("error")) {
            errors.push_back(t.ident + ": " + py_str(d["error"]));
            continue;
        }
        for (const auto& fn : d.value("functions", nlohmann::json::array())) {
            if (fn.contains("status")) ++unencoded;
            for (const auto& vc : fn.value("vcs", nlohmann::json::array())) {
                nlohmann::json row = {{"task", t.ident}, {"function", fn.value("function", "")}};
                for (auto& [k, v] : vc.items()) row[k] = v;
                vcs.push_back(std::move(row));
            }
        }
    }
    std::cerr << tasks.size() << " tasks, " << vcs.size() << " VCs (" << unencoded << " functions not encoded)\n";
    std::vector<std::string> passes;
    for (auto& p : PASSES)
        if (("," + passes_arg + ",").find("," + p + ",") != std::string::npos) passes.push_back(p);

    std::vector<std::map<std::string, nlohmann::json>> res(vcs.size());
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        for (;;) {
            const auto i = next.fetch_add(1);
            if (i >= vcs.size()) return;
            // the passes of one VC run back to back inside one worker
            for (const auto& p : passes) {
                const auto hist = p == "z3"          ? work.path / "hist-z3"
                                  : p == "portfolio" ? work.path / "hist-none" / std::to_string(i)
                                                     : work.path / "hist-online";
                res[i][p] = solve(*prism, vcs[i].value("path", ""), p == "z3", timeout, hist, repo);
            }
        }
    };
    {
        std::vector<std::jthread> pool;
        for (int k = 0; k < jobs; ++k) pool.emplace_back(worker);
    }
    for (const auto& p : passes) {
        double s = 0;
        for (auto& r : res) s += charged(r[p], timeout);
        std::cerr << "pass " << p << ": " << std::format("{:.2f}", s) << " s\n";
    }
    std::vector<nlohmann::json> rows, disagreements;
    for (std::size_t i = 0; i < vcs.size(); ++i) {
        nlohmann::json row = nlohmann::json::object();
        for (auto* k : {"task", "function", "kind", "prop", "line"}) row[k] = vcs[i].value(k, nlohmann::json());
        std::set<std::string> definitive;
        for (const auto& p : passes) {
            const auto& r = res[i][p];
            row[p] = {{"kind", r.value("kind", nlohmann::json())},
                      {"wall_s", r.value("wall_s", nlohmann::json())},
                      {"winner", r.value("winner", nlohmann::json(""))},
                      {"process_s", r.value("process_s", nlohmann::json())}};
            const auto k = r.value("kind", nlohmann::json());
            if (k == "sat" || k == "unsat") definitive.insert(k.get<std::string>());
        }
        if (definitive.size() > 1) disagreements.push_back(row);
        rows.push_back(std::move(row));
    }
    nlohmann::json summary = {{"tasks", tasks.size()},  {"vcs", vcs.size()},    {"unencoded_functions", unencoded},
                              {"timeout_s", timeout},    {"unwind", unwind},     {"errors", errors},
                              {"disagreements", disagreements.size()},           {"passes", nlohmann::json::object()}};
    for (const auto& p : passes) {
        double total = 0, proc = 0, mx = 0;
        int solved = 0, sat = 0, unsat = 0, tos = 0, other = 0;
        std::map<std::string, int> wins;
        for (auto& rr : res) {
            const auto& r = rr[p];
            const double c = charged(r, timeout);
            total += c;
            mx = std::max(mx, c);
            auto ps = r.value("process_s", nlohmann::json(0.0));
            proc += ps.is_number() ? ps.get<double>() : 0.0;
            const auto k = r.value("kind", nlohmann::json());
            if (k == "sat" || k == "unsat") ++solved;
            if (k == "sat") ++sat;
            else if (k == "unsat") ++unsat;
            else if (k == "timeout") ++tos;
            else ++other;
            auto w = r.value("winner", nlohmann::json(""));
            if (w.is_string() && !w.get<std::string>().empty()) ++wins[w.get<std::string>()];
        }
        nlohmann::json wj = nlohmann::json::object();
        for (auto& [k, v] : wins) wj[k] = v;
        summary["passes"][p] = {{"total_s", round_to(total, 3)}, {"process_total_s", round_to(proc, 3)},
                                {"solved", solved},              {"sat", sat},
                                {"unsat", unsat},                {"timeouts", tos},
                                {"other", other},                {"max_s", round_to(mx, 3)},
                                {"winners", wj}};
    }
    auto max_wall = [&](const nlohmann::json& r) {
        double m = 0;
        for (const auto& p : passes) {
            const auto& w = r[p]["wall_s"];
            m = std::max(m, w.is_number() ? w.get<double>() : 0.0);
        }
        return m;
    };
    auto slow = rows;
    std::stable_sort(slow.begin(), slow.end(), [&](auto& a, auto& b) { return max_wall(a) > max_wall(b); });
    if (slow.size() > 15) slow.resize(15);
    summary["slowest"] = slow;
    std::ofstream(out / "solver_bench.json", std::ios::binary)
        << nlohmann::json{{"summary", summary}, {"rows", rows}, {"disagreements", disagreements}}.dump(1);
    std::vector<std::string> md = {
        "# pir VCs: portfolio vs Z3 alone",
        "",
        "- " + std::to_string(tasks.size()) + " conformance tasks, **" + std::to_string(vcs.size()) + " VCs** (" +
            std::to_string(unencoded) + " functions not encoded), unwind " + std::to_string(unwind) + ", timeout " +
            std::format("{:g}", timeout) + " s per VC",
        "- disagreements between passes (must be 0): **" + std::to_string(disagreements.size()) + "**",
        "",
        "| pass | total solver s | total process s | solved | sat | unsat | timeouts | other | max s | winners |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---|"};
    for (const auto& p : passes) {
        const auto& s = summary["passes"][p];
        std::vector<std::pair<std::string, int>> w;
        for (auto& [k, v] : s["winners"].items()) w.emplace_back(k, v.get<int>());
        std::stable_sort(w.begin(), w.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::string ws;
        for (auto& [k, v] : w) ws += (ws.empty() ? "" : ", ") + k + " " + std::to_string(v);
        md.push_back("| " + p + " | " + py_str(s["total_s"]) + " | " + py_str(s["process_total_s"]) + " | " +
                     py_str(s["solved"]) + " | " + py_str(s["sat"]) + " | " + py_str(s["unsat"]) + " | " +
                     py_str(s["timeouts"]) + " | " + py_str(s["other"]) + " | " + py_str(s["max_s"]) + " | " + ws +
                     " |");
    }
    md.insert(md.end(), {"", "Slowest VCs (solver seconds per pass):", ""});
    std::string head = "| task | function | VC | ", sep = "|---|---|---|";
    for (std::size_t i = 0; i < passes.size(); ++i) {
        head += (i ? " | " : "") + passes[i];
        sep += "---:|";
    }
    md.push_back(head + " |");
    md.push_back(sep);
    for (const auto& r : slow) {
        std::string line = "| " + py_str(r["task"]) + " | " + py_str(r["function"]) + " | " + py_str(r["prop"]) +
                           "@" + py_str(r["line"]) + " | ";
        for (std::size_t i = 0; i < passes.size(); ++i) {
            const auto& c = r[passes[i]];
            auto cell = py_str(c["wall_s"]) + " " + py_str(c["kind"]) + " " + py_str(c["winner"]);
            while (!cell.empty() && cell.back() == ' ') cell.pop_back();
            line += (i ? " | " : "") + cell;
        }
        md.push_back(line + " |");
    }
    if (!errors.empty()) {
        md.insert(md.end(), {"", "Front-end errors:", ""});
        for (auto& e : errors) md.push_back("- " + e);
    }
    std::string text;
    for (auto& l : md) text += l + "\n";
    std::ofstream(out / "solver_bench.md", std::ios::binary) << text;
    std::cout << text;
    return disagreements.empty() ? 0 : 1;
}

}  // namespace prism::qa
