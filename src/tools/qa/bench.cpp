// bench: PRISM on pinned real-world C/C++ projects, next to cppcheck and
// clang-tidy (docs/EVALUATION.md is written from its output).
//
//   prism-qa bench [--prism BIN] [--projects testdata/bench/projects.json]
//                  [--only a,b] [--cache DIR] [--out DIR] [--jobs 12]
//                  [--timeout S] [--seed N] [--sample N] [--sample-cppcheck N]
//
// For each project of the manifest:
//  1. fetch: `git fetch --depth 1 REPO COMMIT` into the cache (kept between
//     runs; a checkout whose HEAD is not the pinned commit is fetched again);
//  2. stage: the manifest's `files` are copied into <out>/<name>/tree, its
//     `generate` files written there, and a compile_commands.json written for
//     the translation units with the manifest's -std/-I/-D;
//  3. PRISM: `prism TREE --no-llm --jobs J --out <out>/<name>/prism`;
//  4. cppcheck --project=compile_commands.json --enable=warning,portability;
//  5. clang-tidy -p TREE with the fixed CLANG_TIDY_CHECKS on every unit;
//  6. cbmc, ESBMC, Infer: NOTRUN rows with the reason (Law 1: a baseline
//     that did not run is never a clean result).
// Wall time is measured per tool with the 1-minute load average before, during
// (sampled every 5 s) and after. Metrics: KLOC (non-blank lines of C/C++
// source), PRISM rows by stage and status, a final status per function,
// defects per KLOC, and the overlap of PRISM's defect rows with the
// baselines' by file:line:kind and by file:line. A fixed-seed sample of PRISM
// and cppcheck defect rows, with the source around each, is written for hand
// triage (testdata/bench/triage.md).
//
// Output: <out>/bench.json, <out>/bench.md, <out>/sample-prism.md,
// <out>/sample-cppcheck.md and every tool's raw output under <out>/<name>/.

#include "prism/exec.hpp"
#include "prism/exit.hpp"
#include "prism/log.hpp"
#include "prism/verdict.hpp"
#include "qa.hpp"
#include "support/cli.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <sys/utsname.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace prism::qa {

namespace {

// The fixed clang-tidy check list: the static analyzer and bugprone, minus
// the checks that report style or API-choice rather than a defect.
const std::string CLANG_TIDY_CHECKS =
    "-*,clang-analyzer-*,-clang-analyzer-security.insecureAPI.*,-clang-analyzer-optin.*,"
    "bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-reserved-identifier,"
    "-bugprone-narrowing-conversions,-bugprone-assignment-in-if-condition,-bugprone-branch-clone,"
    "-bugprone-macro-parentheses,-bugprone-implicit-widening-of-multiplication-result,"
    "-bugprone-easily-swappable-parameters,-bugprone-suspicious-include,-bugprone-switch-missing-default-case";
const std::vector<std::string> CPPCHECK_ARGS = {"--enable=warning,portability", "--inconclusive",
                                                "--suppress=missingIncludeSystem", "--quiet"};
// PRISM stages that wrap a baseline tool: left out of the overlap and of the
// "own" defect count (they are reported in the all-stages count).
const std::set<std::string> WRAPPER_STAGES = {"cppcheck", "optional"};

struct Project {
    std::string name, repo, tag, commit, shape, std = "c99", cxxstd = "c++17";
    std::vector<std::string> files, exclude, include, define;
    std::vector<std::pair<std::string, std::string>> generate;
};

struct Row {  // one defect row of any tool, in tree-relative coordinates
    std::string project, tool, stage, status, cls, file, function, message, extra, severity;
    int line = 0;
};

// A defect row `--fail-on defect` counts: not extra.severity warning/note/style.
bool fails(const Row& r) { return r.severity != "warning" && r.severity != "note" && r.severity != "style"; }
// PRISM's own analysis defects: not a baseline wrapper, and failing.
bool own(const Row& r) { return !WRAPPER_STAGES.contains(r.stage) && fails(r); }

std::vector<std::string> strs(const json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key))
        for (const auto& v : j.at(key)) out.push_back(v.get<std::string>());
    return out;
}

std::vector<Project> load_projects(const fs::path& p) {
    auto j = load_json(p);
    if (!j || !j->contains("projects")) throw std::runtime_error("cannot read " + p.string());
    std::vector<Project> out;
    for (const auto& e : j->at("projects")) {
        Project pr;
        pr.name = e.at("name");
        pr.repo = e.at("repo");
        pr.tag = e.value("tag", "");
        pr.commit = e.at("commit");
        pr.shape = e.value("shape", "");
        pr.std = e.value("std", pr.std);
        pr.cxxstd = e.value("cxxstd", pr.cxxstd);
        pr.files = strs(e, "files");
        pr.exclude = strs(e, "exclude");
        pr.include = strs(e, "include");
        pr.define = strs(e, "define");
        if (e.contains("generate"))
            for (const auto& [k, v] : e.at("generate").items()) pr.generate.emplace_back(k, v.get<std::string>());
        out.push_back(std::move(pr));
    }
    return out;
}

bool glob_match(std::string_view pat, std::string_view s) {
    std::size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == s[i])) {
            ++p;
            ++i;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

// Files of `root` matching `pattern` ("dir/sub/*.c": the wildcard is in the
// last component only), sorted.
std::vector<fs::path> expand(const fs::path& root, const std::string& pattern) {
    const fs::path pat(pattern);
    const auto dir = root / pat.parent_path();
    const auto leaf = pat.filename().string();
    std::vector<fs::path> out;
    std::error_code ec;
    if (leaf.find_first_of("*?") == std::string::npos) {
        if (fs::is_regular_file(dir / leaf, ec)) out.push_back(dir / leaf);
        return out;
    }
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && glob_match(leaf, e.path().filename().string())) out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

bool is_source(const fs::path& p) {
    static const std::set<std::string> EXT = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".hxx", ".inl"};
    return EXT.contains(p.extension().string());
}
bool is_unit(const fs::path& p) {
    const auto e = p.extension().string();
    return e == ".c" || e == ".cc" || e == ".cpp" || e == ".cxx";
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

prism::exec::Result run_cmd(const std::vector<std::string>& argv, const fs::path& cwd = {}, double timeout_s = 600,
                            const fs::path& log = {}) {
    prism::exec::Spec s;
    s.argv = argv;
    if (!cwd.empty()) s.cwd = cwd.string();
    s.timeout_ms = static_cast<std::uint64_t>(timeout_s * 1000);
    s.new_session = true;
    if (!log.empty()) {
        s.stdout_file = log.string();
        s.merge_stderr = true;
    }
    return prism::exec::run(s);
}

std::string which(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
    std::stringstream ss(path);
    for (std::string d; std::getline(ss, d, ':');) {
        std::error_code ec;
        auto c = fs::path(d) / name;
        if (fs::is_regular_file(c, ec)) return c.string();
    }
    return {};
}

std::string first_line(const std::string& s) { return trim(s.substr(0, s.find('\n'))); }

// ---- machine and load
double loadavg1() {
    std::ifstream in("/proc/loadavg");
    double v = -1;
    in >> v;
    return v;
}

json machine() {
    json m;
    m["cores"] = std::thread::hardware_concurrency();
    std::ifstream cpu("/proc/cpuinfo");
    for (std::string l; std::getline(cpu, l);)
        if (l.starts_with("model name")) {
            m["cpu"] = trim(l.substr(l.find(':') + 2));
            break;
        }
    std::ifstream mem("/proc/meminfo");
    for (std::string l; std::getline(mem, l);)
        if (l.starts_with("MemTotal:")) {
            std::istringstream is(l.substr(9));
            long long kb = 0;
            is >> kb;
            m["mem_gib"] = std::round(kb / 1048576.0 * 10) / 10;
        }
#ifndef _WIN32
    utsname u{};
    if (uname(&u) == 0) m["kernel"] = std::string(u.sysname) + " " + u.release;
#endif
    return m;
}

struct Timed {
    prism::exec::Result r;
    double load_before = 0, load_mean = 0, load_max = 0, load_after = 0;
};

Timed timed(const std::vector<std::string>& argv, const fs::path& cwd, double timeout_s, const fs::path& log) {
    Timed t;
    t.load_before = loadavg1();
    std::atomic<bool> done{false};
    std::vector<double> samples;
    std::mutex mu;
    std::thread sampler([&] {
        while (!done) {
            for (int i = 0; i < 50 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (done) break;
            std::lock_guard g(mu);
            samples.push_back(loadavg1());
        }
    });
    t.r = run_cmd(argv, cwd, timeout_s, log);
    done = true;
    sampler.join();
    t.load_after = loadavg1();
    samples.push_back(t.load_after);
    double sum = 0;
    for (double s : samples) {
        sum += s;
        t.load_max = std::max(t.load_max, s);
    }
    t.load_mean = samples.empty() ? t.load_after : sum / samples.size();
    return t;
}

json timing_json(const Timed& t) {
    auto r1 = [](double v) { return std::round(v * 10) / 10; };
    return {{"wall_s", r1(t.r.wall_ms / 1000.0)},
            {"rc", t.r.rc()},
            {"timed_out", t.r.timed_out},
            {"failed_to_start", t.r.failed},
            {"peak_rss_mib", t.r.peak_rss_bytes / 1048576},
            {"load_before", r1(t.load_before)},
            {"load_mean", r1(t.load_mean)},
            {"load_max", r1(t.load_max)},
            {"load_after", r1(t.load_after)}};
}

// ---- fetch and stage
std::string fetch(const Project& p, const fs::path& cache) {
    const auto dir = cache / p.name;
    std::error_code ec;
    auto head = [&] { return first_line(run_cmd({"git", "-C", dir.string(), "rev-parse", "HEAD"}).out); };
    if (fs::exists(dir / ".git", ec) && head() == p.commit) return {};
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    for (const auto& argv : std::vector<std::vector<std::string>>{
             {"git", "-C", dir.string(), "init", "-q"},
             {"git", "-C", dir.string(), "fetch", "-q", "--depth", "1", p.repo, p.commit},
             {"git", "-C", dir.string(), "checkout", "-q", "FETCH_HEAD"}}) {
        auto r = run_cmd(argv, {}, 900);
        if (r.rc() != 0) return "`" + argv[3] + "` failed: " + first_line(r.err.empty() ? r.out : r.err);
    }
    if (head() != p.commit) return "HEAD is not the pinned commit " + p.commit;
    return {};
}

struct Staged {
    std::vector<std::string> files, units;  // tree-relative
    long long lines = 0;                    // non-blank lines of C/C++ source
};

Staged stage(const Project& p, const fs::path& src, const fs::path& tree) {
    std::error_code ec;
    fs::remove_all(tree, ec);
    fs::create_directories(tree, ec);
    Staged st;
    std::set<std::string> seen;
    for (const auto& pat : p.files)
        for (const auto& f : expand(src, pat)) {
            const auto rel = fs::relative(f, src, ec).generic_string();
            const auto leaf = f.filename().string();
            bool skip = false;
            for (const auto& x : p.exclude) skip = skip || glob_match(x, rel) || glob_match(x, leaf);
            if (skip || !seen.insert(rel).second) continue;
            fs::create_directories((tree / rel).parent_path(), ec);
            fs::copy_file(f, tree / rel, fs::copy_options::overwrite_existing, ec);
        }
    for (const auto& [name, text] : p.generate) {
        fs::create_directories((tree / name).parent_path(), ec);
        std::ofstream(tree / name, std::ios::binary) << text;
        seen.insert(name);
    }
    st.files.assign(seen.begin(), seen.end());
    json db = json::array();
    for (const auto& rel : st.files) {
        if (!is_source(rel)) continue;
        std::ifstream in(tree / rel, std::ios::binary);
        for (std::string l; std::getline(in, l);)
            if (l.find_first_not_of(" \t\r\f\v") != std::string::npos) ++st.lines;
        if (!is_unit(rel)) continue;
        st.units.push_back(rel);
        const bool c = fs::path(rel).extension() == ".c";
        std::vector<std::string> args{c ? "clang" : "clang++", "-std=" + (c ? p.std : p.cxxstd)};
        for (const auto& i : p.include) args.push_back("-I" + (tree / i).lexically_normal().string());
        for (const auto& d : p.define) args.push_back("-D" + d);
        args.insert(args.end(), {"-c", (tree / rel).string(), "-o", (tree / rel).string() + ".o"});
        db.push_back({{"directory", tree.string()}, {"file", (tree / rel).string()}, {"arguments", args}});
    }
    std::ofstream(tree / "compile_commands.json", std::ios::binary) << db.dump(1) << "\n";
    return st;
}

std::string rel_to(const std::string& file, const fs::path& tree) {
    if (file.empty()) return file;
    fs::path p(file);
    if (p.is_absolute()) {
        std::error_code ec;
        auto r = fs::relative(p, tree, ec);
        if (!ec && !r.empty() && !r.generic_string().starts_with("..")) return r.generic_string();
        return p.generic_string();
    }
    return p.lexically_normal().generic_string();
}

// Coarse defect kind shared by the three tools, from a class / check id / message.
std::string kind_of(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto has = [&](std::initializer_list<const char*> ks) {
        for (auto k : ks)
            if (s.find(k) != std::string::npos) return true;
        return false;
    };
    if (has({"uninit"})) return "uninit";
    if (has({"leak"})) return "leak";
    if (has({"double-free", "doublefree", "use-after-free", "useafterfree", "deallocuse", "dangl", "after-free",
             "mismatchallocdealloc", "unix.malloc"}))
        return "free";
    if (has({"null"})) return "null";
    if (has({"div-zero", "divzero", "zerodiv", "dividezero", "division by zero", "div0"})) return "divzero";
    if (has({"oob", "outofbounds", "out-of-bound", "bounds", "arrayindex", "buffer", "overrun", "bufferaccess"}))
        return "oob";
    if (has({"ovf", "overflow", "shift", "wrap"})) return "intovf";
    return "other";
}

// ---- PRISM report
struct PrismStats {
    json by_stage = json::object();   // stage -> status -> rows
    json by_status = json::object();  // status -> rows
    json functions = json::object();  // final status -> functions
    long long n_functions = 0;
    std::vector<Row> defects;         // every stage
    json stages = json::array();      // stage name, status, seconds
    double confidence = 0;
};

// Final status of a function from every row naming it: a defect, else a
// proof (any PROVED-*), else BOUNDED, else NEEDS-HARNESS, else another
// no-answer (UNKNOWN/TIMEOUT/ERROR/NOFUNC), else NOTRUN, else no row.
int final_rank(const std::string& st) {
    if (st == "FAILED" || st == "CRASH" || st == "SANFAIL") return 7;
    if (st.starts_with("PROVED")) return 6;
    if (st == "BOUNDED") return 5;
    if (st == "NEEDS-HARNESS") return 4;
    if (st == "UNKNOWN" || st == "TIMEOUT" || st == "ERROR" || st == "NOFUNC") return 3;
    if (st == "NOTRUN") return 2;
    return 0;
}
const char* final_name(int r) {
    static const char* N[] = {"no row", "no row", "NOTRUN", "no answer", "NEEDS-HARNESS", "BOUNDED", "PROVED",
                              "FAILED"};
    return N[r];
}

// report.json, else (a run that was killed) the stages journal
// stages.jsonl and functions.json; `partial` says which.
std::optional<json> load_report(const fs::path& dir, bool& partial) {
    partial = false;
    if (auto j = load_json(dir / "report.json")) return j;
    std::ifstream in(dir / "stages.jsonl", std::ios::binary);
    if (!in) return std::nullopt;
    json j = {{"stages", json::array()}, {"functions", load_json(dir / "functions.json").value_or(json::array())}};
    for (std::string l; std::getline(in, l);) {
        try {
            auto s = json::parse(l);
            if (s.is_object() && s.contains("name")) j["stages"].push_back(std::move(s));
        } catch (const std::exception&) {
        }
    }
    partial = true;
    return j;
}

std::optional<PrismStats> prism_stats(const std::string& project, const fs::path& dir, const fs::path& tree,
                                      bool& partial) {
    auto j = load_report(dir, partial);
    if (!j) return std::nullopt;
    PrismStats s;
    s.confidence = j->value("confidence", 0.0);
    std::map<std::string, int> fn_rank;
    auto key = [&](const std::string& f, const std::string& n) { return rel_to(f, tree) + "::" + n; };
    if (j->contains("functions"))
        for (const auto& f : j->at("functions")) fn_rank[key(f.value("file", ""), f.value("name", ""))] = 0;
    s.n_functions = static_cast<long long>(fn_rank.size());
    for (const auto& st : j->value("stages", json::array())) {
        const std::string sname = st.value("name", "");
        s.stages.push_back({{"stage", sname},
                            {"status", st.value("status", "")},
                            {"seconds", std::round(st.value("elapsed", 0.0) * 10) / 10}});
        for (const auto& f : st.value("findings", json::array())) {
            const std::string status = f.value("status", "");
            if (!s.by_stage.contains(sname)) s.by_stage[sname] = json::object();
            s.by_stage[sname][status] = s.by_stage[sname].value(status, 0) + 1;
            s.by_status[status] = s.by_status.value(status, 0) + 1;
            const std::string fn = f.contains("function") && f["function"].is_string() ? f["function"].get<std::string>() : "";
            if (!fn.empty()) {
                auto it = fn_rank.find(key(f.value("file", ""), fn));
                if (it != fn_rank.end()) it->second = std::max(it->second, final_rank(status));
            }
            auto v = prism::verdict::parse(status);
            if (!v || !prism::verdict::is_defect(*v)) continue;
            Row r;
            r.project = project;
            r.tool = "prism";
            r.stage = sname;
            r.status = status;
            r.cls = f.value("cls", f.value("class", ""));
            r.file = rel_to(f.value("file", ""), tree);
            r.function = fn;
            r.line = f.contains("line") && f["line"].is_number_integer() ? f["line"].get<int>() : 0;
            r.message = f.value("message", "");
            r.extra = f.value("counterexample", "");
            if (f.contains("extra") && f["extra"].is_object()) r.severity = f["extra"].value("severity", "");
            s.defects.push_back(std::move(r));
        }
    }
    for (const auto& [k, r] : fn_rank) s.functions[final_name(r)] = s.functions.value(final_name(r), 0) + 1;
    return s;
}

// ---- baselines
std::vector<Row> parse_cppcheck(const std::string& project, const std::string& text, const fs::path& tree) {
    // {file}\x1f{line}\x1f{column}\x1f{severity}\x1f{id}\x1f{message}
    std::vector<Row> out;
    std::istringstream in(text);
    std::set<std::string> seen;
    for (std::string l; std::getline(in, l);) {
        std::vector<std::string> f;
        std::size_t a = 0;
        for (int i = 0; i < 5; ++i) {
            auto b = l.find('\x1f', a);
            if (b == std::string::npos) break;
            f.push_back(l.substr(a, b - a));
            a = b + 1;
        }
        if (f.size() != 5) continue;
        f.push_back(trim(l.substr(a)));
        if (f[3] == "information" || f[3] == "none" || f[4] == "checkersReport") continue;
        Row r;
        r.project = project;
        r.tool = "cppcheck";
        r.stage = "cppcheck";
        r.status = f[3];
        r.cls = f[4];
        r.file = rel_to(f[0], tree);
        r.line = std::atoi(f[1].c_str());
        r.message = f[5];
        if (r.file.empty() || fs::path(r.file).is_absolute()) continue;  // a system header
        if (!seen.insert(std::format("{}:{}:{}", r.file, r.line, r.cls)).second) continue;
        out.push_back(std::move(r));
    }
    return out;
}

struct TidyOut {
    std::vector<Row> rows;
    int compile_errors = 0;
};

TidyOut parse_tidy(const std::string& project, const std::string& text, const fs::path& tree,
                   std::set<std::string>& seen) {
    TidyOut t;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) {
        l = trim(l);
        std::size_t at = std::string::npos;
        std::string sev;
        for (const char* s : {": warning: ", ": error: "})
            if (auto p = l.find(s); p != std::string::npos && p < at) {
                at = p;
                sev = std::string(s).substr(2, std::string(s).size() - 4);
            }
        if (at == std::string::npos || l.back() != ']') continue;
        const auto lb = l.rfind(" [");
        if (lb == std::string::npos || lb < at) continue;
        const auto check = l.substr(lb + 2, l.size() - lb - 3);
        const auto loc = l.substr(0, at);  // file:line:col
        const auto c2 = loc.rfind(':');
        const auto c1 = c2 == std::string::npos ? std::string::npos : loc.rfind(':', c2 - 1);
        if (c1 == std::string::npos) continue;
        if (check.starts_with("clang-diagnostic-")) {
            if (sev == "error") ++t.compile_errors;
            continue;
        }
        Row r;
        r.project = project;
        r.tool = "clang-tidy";
        r.stage = "clang-tidy";
        r.status = sev;
        r.cls = check;
        r.file = rel_to(loc.substr(0, c1), tree);
        r.line = std::atoi(loc.substr(c1 + 1, c2 - c1 - 1).c_str());
        r.message = l.substr(at + 2 + sev.size() + 2, lb - (at + 2 + sev.size() + 2));
        if (r.file.empty() || fs::path(r.file).is_absolute()) continue;
        if (!seen.insert(std::format("{}:{}:{}", r.file, r.line, r.cls)).second) continue;
        t.rows.push_back(std::move(r));
    }
    return t;
}

std::set<std::string> keys(const std::vector<Row>& rows, bool with_kind, bool own_only) {
    std::set<std::string> k;
    for (const auto& r : rows) {
        if (r.line <= 0 || r.file.empty()) continue;
        if (own_only && !own(r)) continue;
        k.insert(with_kind ? std::format("{}:{}:{}", r.file, r.line, kind_of(r.cls + " " + r.message))
                           : std::format("{}:{}", r.file, r.line));
    }
    return k;
}

std::size_t inter(const std::set<std::string>& a, const std::set<std::string>& b) {
    std::size_t n = 0;
    for (const auto& x : a) n += b.contains(x);
    return n;
}

// ---- sampling: per project a Fisher-Yates shuffle with mt19937_64 (rng() %
// (i + 1), the same on every standard library), then round-robin over the
// projects in manifest order.
std::vector<Row> sample(const std::vector<std::vector<Row>>& per_project, std::size_t n, std::uint64_t seed) {
    std::vector<std::vector<Row>> pools = per_project;
    std::mt19937_64 rng(seed);
    for (auto& p : pools) {
        std::sort(p.begin(), p.end(), [](const Row& a, const Row& b) {
            return std::tie(a.file, a.line, a.stage, a.cls, a.message) <
                   std::tie(b.file, b.line, b.stage, b.cls, b.message);
        });
        for (std::size_t i = p.size(); i > 1; --i) std::swap(p[i - 1], p[rng() % i]);
    }
    std::vector<Row> out;
    for (std::size_t round = 0; out.size() < n; ++round) {
        bool any = false;
        for (const auto& p : pools)
            if (round < p.size() && out.size() < n) {
                out.push_back(p[round]);
                any = true;
            }
        if (!any) break;
    }
    return out;
}

std::string excerpt(const fs::path& file, int line, int ctx = 4) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream o;
    int n = 0;
    for (std::string l; std::getline(in, l);) {
        ++n;
        if (n < line - ctx) continue;
        if (n > line + ctx) break;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        o << std::format("{}{:>6} | {}\n", n == line ? ">" : " ", n, l);
    }
    return o.str();
}

std::string cut(std::string s, std::size_t n) {
    for (auto& c : s)
        if (c == '\n' || c == '|') c = ' ';
    if (s.size() > n) s = s.substr(0, n) + "...";
    return s;
}

void write_sample(const fs::path& path, const std::string& title, const std::vector<Row>& rows, const fs::path& out,
                  std::uint64_t seed) {
    std::ofstream o(path, std::ios::binary);
    o << "# " << title << "\n\nSeed " << seed << "; " << rows.size()
      << " rows, round-robin over the projects (prism-qa bench).\n\n";
    int i = 0;
    for (const auto& r : rows) {
        o << std::format("## {}. {} `{}:{}` {} {} {}\n\n", ++i, r.project, r.file, r.line, r.stage, r.status, r.cls);
        if (!r.function.empty()) o << "function `" << r.function << "`\n\n";
        o << "> " << cut(r.message, 600) << "\n\n";
        if (!r.extra.empty()) o << "counterexample `" << cut(r.extra, 300) << "`\n\n";
        if (r.line > 0) o << "```\n" << excerpt(out / r.project / "tree" / r.file, r.line) << "```\n\n";
    }
}

std::string pct(long long a, long long b) { return b ? std::format("{:.1f}%", 100.0 * a / b) : "-"; }
std::string per_k(std::size_t n, long long lines) { return lines ? std::format("{:.2f}", n * 1000.0 / lines) : "-"; }

}  // namespace

int bench_main(const Args& argv) {
    const std::vector<ArgSpec> specs = {
        {"--prism", "", true, false, "PRISM binary (default: $PRISM_BIN, else prism next to prism-qa, else build/prism)"},
        {"--projects", "", true, false, "manifest (default: testdata/bench/projects.json)"},
        {"--only", "", true, false, "comma-separated project names (default: all)"},
        {"--cache", "", true, false, "checkout cache (default: ~/.cache/prism/bench)"},
        {"--out", "", true, false, "output directory (default: bench-out)"},
        {"--jobs", "-j", true, false, "--jobs for PRISM, cppcheck and clang-tidy (default 12)"},
        {"--timeout", "", true, false, "seconds per PRISM run (default 5400)"},
        {"--seed", "", true, false, "sample seed (default 20261105)"},
        {"--sample", "", true, false, "PRISM defect rows to sample for triage (default 30)"},
        {"--sample-cppcheck", "", true, false, "cppcheck rows to sample (default 12)"},
    };
    CliArgs args;
    int rc = 0;
    if (!parse_args("prism-qa bench", "PRISM on pinned real-world projects next to cppcheck and clang-tidy.", specs,
                    argv, args, rc))
        return rc;
    int jobs = 12;
    double timeout = 5400;
    std::uint64_t seed = 20261105;
    std::size_t n_sample = 30, n_sample_cpp = 12;
    try {
        if (args.has("--jobs")) jobs = std::max(1, std::stoi(args.get("--jobs")));
        if (args.has("--timeout")) timeout = std::stod(args.get("--timeout"));
        if (args.has("--seed")) seed = std::stoull(args.get("--seed"));
        if (args.has("--sample")) n_sample = std::stoul(args.get("--sample"));
        if (args.has("--sample-cppcheck")) n_sample_cpp = std::stoul(args.get("--sample-cppcheck"));
    } catch (...) {
        return prism::fail("bench", "invalid number");
    }
    const auto repo = repo_root();
    const auto prism_bin = find_prism(args.get("--prism"), repo);
    if (!prism_bin) return prism::fail("bench", "no prism binary (--prism, $PRISM_BIN or build/prism)");
    std::vector<Project> projects;
    try {
        projects = load_projects(args.get("--projects", (repo / "testdata" / "bench" / "projects.json").string()));
    } catch (const std::exception& e) {
        return prism::fail("bench", e.what());
    }
    if (args.has("--only")) {
        std::set<std::string> only;
        std::stringstream ss(args.get("--only"));
        for (std::string n; std::getline(ss, n, ',');) only.insert(n);
        std::erase_if(projects, [&](const Project& p) { return !only.contains(p.name); });
    }
    const char* home = std::getenv("HOME");
    const fs::path cache = args.get("--cache", (fs::path(home ? home : ".") / ".cache" / "prism" / "bench").string());
    const fs::path out = fs::absolute(args.get("--out", "bench-out"));
    std::error_code ec;
    fs::create_directories(out, ec);
    fs::create_directories(cache, ec);

    // Tool versions; a missing baseline is NOTRUN (Law 1).
    auto version = [](std::vector<std::string> argv) {
        auto r = run_cmd(argv, {}, 60);
        return r.rc() == 0 ? first_line(r.out.empty() ? r.err : r.out) : std::string();
    };
    const std::string cppcheck = which("cppcheck"), tidy = which("clang-tidy"), cbmc = which("cbmc");
    json tools = {
        {"prism", {{"path", prism_bin->string()}, {"version", version({prism_bin->string(), "--version"})}}},
        {"cppcheck", {{"path", cppcheck}, {"version", cppcheck.empty() ? "" : version({cppcheck, "--version"})}}},
        {"clang-tidy", {{"path", tidy}, {"version", tidy.empty() ? "" : version({tidy, "--version"})}}},
    };
    auto git = run_cmd({"git", "-C", repo.string(), "rev-parse", "HEAD"});
    auto dirty = run_cmd({"git", "-C", repo.string(), "status", "--porcelain", "--untracked-files=no"});
    json notrun = json::array();
    notrun.push_back({{"tool", "cbmc"},
                      {"status", "NOTRUN"},
                      {"reason", cbmc.empty() ? "not installed"
                                              : "installed (" + version({cbmc, "--version"}) +
                                                    ") but not run: a per-function cbmc run needs a harness per "
                                                    "function (pointer parameters, globals), which the bench does "
                                                    "not generate"}});
    for (const char* t : {"esbmc", "infer"})
        notrun.push_back({{"tool", t},
                          {"status", "NOTRUN"},
                          {"reason", which(t).empty() ? "not installed" : "installed but the bench has no adapter"}});

    const auto t0 = std::time(nullptr);
    char date[32];
    std::strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", std::localtime(&t0));
    json result = {{"date", date},
                   {"prism_commit", first_line(git.out)},
                   {"prism_tree_dirty", !trim(dirty.out).empty()},
                   {"machine", machine()},
                   {"jobs", jobs},
                   {"timeout_s", timeout},
                   {"seed", seed},
                   {"tools", tools},
                   {"clang_tidy_checks", CLANG_TIDY_CHECKS},
                   {"cppcheck_args", CPPCHECK_ARGS},
                   {"baselines_notrun", notrun},
                   {"projects", json::array()}};
    std::vector<std::vector<Row>> prism_rows, cpp_rows;
    for (const auto& p : projects) {
        prism::log::plain(std::format("== {} ({} {})\n", p.name, p.tag, p.commit.substr(0, 12)));
        json pj = {{"name", p.name}, {"repo", p.repo}, {"tag", p.tag}, {"commit", p.commit}, {"shape", p.shape}};
        const auto pdir = out / p.name;
        fs::remove_all(pdir, ec);
        fs::create_directories(pdir, ec);
        if (auto err = fetch(p, cache); !err.empty()) {
            pj["fetch"] = "ERROR: " + err;
            prism::log::error("bench", p.name + ": fetch: " + err);
            result["projects"].push_back(pj);
            prism_rows.emplace_back();
            cpp_rows.emplace_back();
            continue;
        }
        const auto tree = pdir / "tree";
        const auto st = stage(p, cache / p.name, tree);
        pj["files"] = st.files.size();
        pj["units"] = st.units.size();
        pj["lines"] = st.lines;
        pj["kloc"] = std::round(st.lines / 100.0) / 10;

        // PRISM
        const auto prism_out = pdir / "prism";
        auto tp = timed({prism_bin->string(), tree.string(), "--no-llm", "--jobs", std::to_string(jobs), "--out",
                         prism_out.string()},
                        pdir, timeout, pdir / "prism.log");
        pj["prism_run"] = timing_json(tp);
        prism::log::plain(std::format("   prism {:.0f} s rc {}\n", tp.r.wall_ms / 1000.0, tp.r.rc()));
        std::vector<Row> prow;
        bool partial = false;
        if (auto ps = prism_stats(p.name, prism_out, tree, partial)) {
            pj["prism"] = {{"rows_by_stage", ps->by_stage},     {"rows_by_status", ps->by_status},
                           {"functions", ps->n_functions},      {"function_final", ps->functions},
                           {"stages", ps->stages},              {"confidence", ps->confidence},
                           {"defects", ps->defects.size()},
                           {"report", partial ? "partial: no report.json, read stages.jsonl" : "report.json"}};
            std::size_t n_own = 0, n_fail = 0;
            json own_by_stage = json::object();
            for (const auto& r : ps->defects) {
                n_fail += fails(r);
                if (!own(r)) continue;
                ++n_own;
                own_by_stage[r.stage] = own_by_stage.value(r.stage, 0) + 1;
            }
            pj["prism"]["defects_failon"] = n_fail;
            pj["prism"]["defects_own"] = n_own;
            pj["prism"]["defects_own_by_stage"] = own_by_stage;
            prow = std::move(ps->defects);
        } else {
            pj["prism"] = {{"report", "missing: no report.json or stages.jsonl (see prism.log)"}};
        }

        // cppcheck
        std::vector<Row> crow;
        if (!cppcheck.empty()) {
            std::vector<std::string> a{cppcheck, "--project=" + (tree / "compile_commands.json").string(),
                                       "--template={file}\x1f{line}\x1f{column}\x1f{severity}\x1f{id}\x1f{message}",
                                       "-j", std::to_string(jobs)};
            a.insert(a.end(), CPPCHECK_ARGS.begin(), CPPCHECK_ARGS.end());
            auto tc = timed(a, pdir, timeout, pdir / "cppcheck.log");
            crow = parse_cppcheck(p.name, read_text(pdir / "cppcheck.log"), tree);
            pj["cppcheck_run"] = timing_json(tc);
            pj["cppcheck"] = {{"rows", crow.size()}};
            prism::log::plain(std::format("   cppcheck {:.0f} s, {} rows\n", tc.r.wall_ms / 1000.0, crow.size()));
        } else {
            pj["cppcheck"] = {{"status", "NOTRUN"}, {"reason", "cppcheck not installed"}};
        }

        // clang-tidy, one process per unit, `jobs` at a time
        std::vector<Row> trow;
        if (!tidy.empty()) {
            const auto t_start = std::chrono::steady_clock::now();
            const double lb = loadavg1();
            std::vector<std::string> logs(st.units.size());
            std::vector<int> rcs(st.units.size(), 0);
            std::atomic<std::size_t> next{0};
            std::vector<std::thread> pool;
            for (int w = 0; w < jobs; ++w)
                pool.emplace_back([&] {
                    for (std::size_t i; (i = next++) < st.units.size();) {
                        auto r = run_cmd({tidy, "-p", tree.string(), "--checks=" + CLANG_TIDY_CHECKS, "--quiet",
                                          "--header-filter=.*", (tree / st.units[i]).string()},
                                         tree, timeout);
                        logs[i] = r.out + r.err;
                        rcs[i] = r.rc();
                    }
                });
            for (auto& t : pool) t.join();
            std::ofstream tl(pdir / "clang-tidy.log", std::ios::binary);
            std::set<std::string> seen;
            int errs = 0;
            for (std::size_t i = 0; i < logs.size(); ++i) {
                tl << "### " << st.units[i] << " rc " << rcs[i] << "\n" << logs[i];
                auto t = parse_tidy(p.name, logs[i], tree, seen);
                errs += t.compile_errors;
                for (auto& r : t.rows) trow.push_back(std::move(r));
            }
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
            pj["clang_tidy_run"] = {{"wall_s", std::round(wall * 10) / 10},
                                    {"load_before", std::round(lb * 10) / 10},
                                    {"load_after", std::round(loadavg1() * 10) / 10}};
            pj["clang_tidy"] = {{"rows", trow.size()}, {"compile_errors", errs}};
            prism::log::plain(std::format("   clang-tidy {:.0f} s, {} rows\n", wall, trow.size()));
        } else {
            pj["clang_tidy"] = {{"status", "NOTRUN"}, {"reason", "clang-tidy not installed"}};
        }

        // overlap
        const auto pk = keys(prow, true, true), ck = keys(crow, true, false), tk = keys(trow, true, false);
        const auto pl = keys(prow, false, true), cl = keys(crow, false, false), tl = keys(trow, false, false);
        pj["overlap"] = {{"prism_keys", pk.size()},       {"cppcheck_keys", ck.size()},
                         {"clang_tidy_keys", tk.size()},  {"prism_cppcheck", inter(pk, ck)},
                         {"prism_clang_tidy", inter(pk, tk)}, {"cppcheck_clang_tidy", inter(ck, tk)},
                         {"prism_cppcheck_line", inter(pl, cl)}, {"prism_clang_tidy_line", inter(pl, tl)},
                         {"cppcheck_clang_tidy_line", inter(cl, tl)}};
        // rows of every tool, for the record
        json all = json::array();
        for (const auto* v : {&prow, &crow, &trow})
            for (const auto& r : *v)
                all.push_back({{"tool", r.tool}, {"stage", r.stage}, {"status", r.status}, {"cls", r.cls},
                               {"kind", kind_of(r.cls + " " + r.message)}, {"file", r.file}, {"line", r.line},
                               {"function", r.function}, {"message", r.message}});
        std::ofstream(pdir / "defects.json", std::ios::binary) << all.dump(1) << "\n";
        std::vector<Row> mine;
        for (const auto& r : prow)
            if (own(r)) mine.push_back(r);
        prism_rows.push_back(std::move(mine));
        cpp_rows.push_back(crow);
        result["projects"].push_back(pj);
        std::ofstream(out / "bench.json", std::ios::binary) << result.dump(1) << "\n";  // partial results survive
    }

    const auto ps = sample(prism_rows, n_sample, seed);
    const auto cs = sample(cpp_rows, n_sample_cpp, seed);
    write_sample(out / "sample-prism.md", "PRISM defect rows for triage", ps, out, seed);
    write_sample(out / "sample-cppcheck.md", "cppcheck rows for triage", cs, out, seed);
    json sj = json::array();
    for (const auto& r : ps) sj.push_back({{"project", r.project}, {"stage", r.stage}, {"cls", r.cls},
                                           {"file", r.file}, {"line", r.line}});
    result["sample_prism"] = sj;
    sj = json::array();
    for (const auto& r : cs) sj.push_back({{"project", r.project}, {"cls", r.cls}, {"file", r.file}, {"line", r.line}});
    result["sample_cppcheck"] = sj;
    std::ofstream(out / "bench.json", std::ios::binary) << result.dump(1) << "\n";

    // bench.md
    std::ostringstream md;
    md << "# prism-qa bench\n\n" << date << ", PRISM `" << result["prism_commit"].get<std::string>()
       << (result["prism_tree_dirty"].get<bool>() ? " (dirty)" : "") << "`, --jobs " << jobs << ", machine "
       << result["machine"].dump() << "\n\n";
    md << "## Projects\n\n| project | tag | commit | shape | files | units | KLOC |\n|---|---|---|---|---:|---:|---:|\n";
    for (const auto& pj : result["projects"])
        md << std::format("| {} | {} | `{}` | {} | {} | {} | {} |\n", pj["name"].get<std::string>(),
                          pj["tag"].get<std::string>(), pj["commit"].get<std::string>().substr(0, 12),
                          pj["shape"].get<std::string>(), pj.value("files", 0), pj.value("units", 0),
                          pj.value("kloc", 0.0));
    md << "\n## Wall time (s) and 1-minute load average (before / mean / max during PRISM)\n\n"
          "| project | PRISM | rc | load | peak RSS MiB | cppcheck | clang-tidy |\n|---|---:|---:|---|---:|---:|---:|\n";
    for (const auto& pj : result["projects"]) {
        if (!pj.contains("prism_run")) continue;
        const auto& r = pj["prism_run"];
        md << std::format("| {} | {} | {}{} | {} / {} / {} | {} | {} | {} |\n", pj["name"].get<std::string>(),
                          r["wall_s"].dump(), r["rc"].dump(), r["timed_out"].get<bool>() ? " TIMEOUT" : "",
                          r["load_before"].dump(), r["load_mean"].dump(), r["load_max"].dump(),
                          r["peak_rss_mib"].dump(),
                          pj.contains("cppcheck_run") ? pj["cppcheck_run"]["wall_s"].dump() : "NOTRUN",
                          pj.contains("clang_tidy_run") ? pj["clang_tidy_run"]["wall_s"].dump() : "NOTRUN");
    }
    // statuses seen anywhere, in a fixed order
    std::vector<std::string> statuses;
    for (auto v : prism::verdict::all_verdicts()) statuses.emplace_back(prism::verdict::name(v));
    md << "\n## PRISM rows by status\n\n| project |";
    for (const auto& s : statuses) md << " " << s << " |";
    md << "\n|---|";
    for (std::size_t i = 0; i < statuses.size(); ++i) md << "---:|";
    md << "\n";
    for (const auto& pj : result["projects"]) {
        if (!pj.contains("prism") || !pj["prism"].contains("rows_by_status")) continue;
        md << "| " << pj["name"].get<std::string>() << " |";
        for (const auto& s : statuses) md << " " << pj["prism"]["rows_by_status"].value(s, 0) << " |";
        md << "\n";
    }
    const std::vector<std::string> finals = {"PROVED", "BOUNDED", "FAILED", "NEEDS-HARNESS", "no answer", "NOTRUN",
                                             "no row"};
    md << "\n## Final status per function (share of functions)\n\nA function is FAILED when any row names it with "
          "a defect, else PROVED on any PROVED-* row, else BOUNDED, NEEDS-HARNESS, another no-answer "
          "(UNKNOWN/TIMEOUT/ERROR/NOFUNC), NOTRUN, in that order.\n\n| project | functions |";
    for (const auto& f : finals) md << " " << f << " |";
    md << "\n|---|---:|";
    for (std::size_t i = 0; i < finals.size(); ++i) md << "---:|";
    md << "\n";
    for (const auto& pj : result["projects"]) {
        if (!pj.contains("prism") || !pj["prism"].contains("functions")) continue;
        const long long n = pj["prism"]["functions"].get<long long>();
        md << "| " << pj["name"].get<std::string>() << " | " << n << " |";
        for (const auto& f : finals) {
            const long long k = pj["prism"]["function_final"].value(f, 0LL);
            md << " " << k << " (" << pct(k, n) << ") |";
        }
        md << "\n";
    }
    md << "\n## Defects per KLOC and overlap (file:line:kind; file:line in brackets)\n\n"
          "PRISM all = every FAILED/CRASH/SANFAIL row; fail-on = those `--fail-on defect` counts (not "
          "extra.severity warning/note/style, which drops the compiler-warning rows); own = fail-on rows of every "
          "stage except `cppcheck` and `optional`, which wrap the baselines. Overlap and /KLOC use own.\n\n"
          "| project | KLOC | PRISM all | fail-on | own | /KLOC | cppcheck | /KLOC | clang-tidy | /KLOC | P∩C | P∩T | "
          "C∩T |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& pj : result["projects"]) {
        if (!pj.contains("overlap")) continue;
        const long long lines = pj["lines"].get<long long>();
        const std::size_t all = pj["prism"].value("defects", 0), own = pj["prism"].value("defects_own", 0);
        const std::size_t fo = pj["prism"].value("defects_failon", 0);
        const std::size_t c = pj["cppcheck"].value("rows", 0), t = pj["clang_tidy"].value("rows", 0);
        const auto& o = pj["overlap"];
        md << std::format("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} ({}) | {} ({}) | {} ({}) |\n",
                          pj["name"].get<std::string>(), pj["kloc"].dump(), all, fo, own, per_k(own, lines), c,
                          per_k(c, lines), t, per_k(t, lines), o["prism_cppcheck"].dump(),
                          o["prism_cppcheck_line"].dump(), o["prism_clang_tidy"].dump(),
                          o["prism_clang_tidy_line"].dump(), o["cppcheck_clang_tidy"].dump(),
                          o["cppcheck_clang_tidy_line"].dump());
    }
    md << "\n## PRISM own defect rows by stage\n\n| project | rows |\n|---|---|\n";
    for (const auto& pj : result["projects"]) {
        if (!pj.contains("prism") || !pj["prism"].contains("defects_own_by_stage")) continue;
        std::string cells;
        for (const auto& [k, v] : pj["prism"]["defects_own_by_stage"].items())
            cells += (cells.empty() ? "" : ", ") + k + " " + v.dump();
        md << "| " << pj["name"].get<std::string>() << " | " << cells << " |\n";
    }
    md << "\n## Baselines not run (Law 1)\n\n";
    for (const auto& n : notrun)
        md << "- " << n["tool"].get<std::string>() << ": NOTRUN, " << n["reason"].get<std::string>() << "\n";
    md << "\n## PRISM rows by stage and status\n";
    for (const auto& pj : result["projects"]) {
        if (!pj.contains("prism") || !pj["prism"].contains("rows_by_stage")) continue;
        md << "\n### " << pj["name"].get<std::string>() << "\n\n| stage | seconds | rows |\n|---|---:|---|\n";
        for (const auto& s : pj["prism"]["stages"]) {
            const auto name = s["stage"].get<std::string>();
            std::string cells;
            if (pj["prism"]["rows_by_stage"].contains(name))
                for (const auto& [k, v] : pj["prism"]["rows_by_stage"][name].items())
                    cells += (cells.empty() ? "" : ", ") + k + " " + v.dump();
            md << "| " << name << " | " << s["seconds"].dump() << " | " << cells << " |\n";
        }
    }
    std::ofstream(out / "bench.md", std::ios::binary) << md.str();
    prism::log::plain("wrote " + (out / "bench.md").string() + "\n");
    return 0;
}

}  // namespace prism::qa
