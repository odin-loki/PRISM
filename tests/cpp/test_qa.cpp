// prism-qa (src/tools/qa) and the session runner it uses
// (src/prism/proc_session.cpp). Part of prism_tests.
//
// A tool that needs something this machine lacks (the Lean checkers built by
// lake, lli) says NOTRUN in a doctest MESSAGE and checks what it can without
// it; nothing is skipped quietly.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "../../src/prism/proc.hpp"
#include "../../src/tools/qa/qa.hpp"

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/pir.hpp"
#include "prism/shipdocs.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path repo() { return prism::qa::repo_root(); }

// The binaries CMake builds next to prism_tests (add_dependencies).
fs::path sibling(const char* name) {
    std::error_code ec;
    auto exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path(name) : exe.parent_path() / name;
}

struct Tmp {
    fs::path path;
    explicit Tmp(const std::string& prefix) {
        path = fs::temp_directory_path() /
               (prefix + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

bool has_line(const std::string& text, const std::string& line) {
    auto ls = lines_of(text);
    return std::find(ls.begin(), ls.end(), line) != ls.end();
}

#ifndef _WIN32
// A stand-in for PRISM: prism_tests re-run with PRISM_QA_FAKE_SOLVER_PIDFILE
// set starts a "solver" (sleep 60) in a process group of its own, as
// src/prism/solver/util.cpp does, writes its pid to the file and waits.
// Runs before doctest's main, so the child never reaches the test runner.
struct FakePrismHook {
    FakePrismHook() {
        const char* pidfile = std::getenv("PRISM_QA_FAKE_SOLVER_PIDFILE");
        if (!pidfile || !*pidfile) return;
        const pid_t g = ::fork();
        if (g == 0) {
            ::setpgid(0, 0);
            ::execlp("sleep", "sleep", "60", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        if (g > 0) ::setpgid(g, g);
        {
            std::ofstream(std::string(pidfile) + ".tmp") << g;
        }
        std::rename((std::string(pidfile) + ".tmp").c_str(), pidfile);
        ::sleep(60);
        ::_exit(0);
    }
} fake_prism_hook;

long solver_pid(const fs::path& pidfile) {
    for (int i = 0; i < 500; ++i) {
        std::ifstream in(pidfile);
        long v = 0;
        if (in >> v && v > 1) return v;
        ::usleep(20000);
    }
    return 0;
}

// the process exited (absent, or a zombie waiting for its reaper)
bool gone(long pid) {
    for (int i = 0; i < 250; ++i) {
        std::ifstream st("/proc/" + std::to_string(pid) + "/stat");
        if (!st) return true;
        std::string s;
        try {
            s.assign(std::istreambuf_iterator<char>(st), std::istreambuf_iterator<char>());
        } catch (const std::exception&) {
            return true;  // ESRCH mid-read: the process went away
        }
        auto rp = s.rfind(')');
        if (s.empty() || (rp != std::string::npos && rp + 2 < s.size() && s[rp + 2] == 'Z')) return true;
        ::usleep(20000);
    }
    return false;
}

prism::detail::SessionOptions fake_prism(const fs::path& pidfile, double timeout) {
    prism::detail::SessionOptions so;
    so.env = {{"PRISM_QA_FAKE_SOLVER_PIDFILE", pidfile.string()}};
    so.timeout_s = timeout;
    return so;
}
#endif

}  // namespace

// ---------------------------------------------------------------- sessions
#ifndef _WIN32
TEST_CASE("session_members sees a solver in a process group of its own; kill_session kills it") {
    Tmp t("prism_qa_sess_");
    const auto pidfile = t.path / "solver.pid";
    const pid_t p = ::fork();
    REQUIRE(p >= 0);
    if (p == 0) {
        ::setsid();
        ::setenv("PRISM_QA_FAKE_SOLVER_PIDFILE", pidfile.c_str(), 1);
        ::execl(sibling("prism_tests").c_str(), "prism_tests", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    const long solver = solver_pid(pidfile);
    REQUIRE(solver > 1);
    CHECK(::getpgid(static_cast<pid_t>(solver)) != p);  // its own group ...
    auto members = prism::detail::session_members(p);
    CHECK(std::find(members.begin(), members.end(), static_cast<int>(solver)) != members.end());  // ... same session
    prism::detail::kill_session(p);
    int st = 0;
    ::waitpid(p, &st, 0);
    CHECK(gone(solver));
}

TEST_CASE("run_session: a timeout kills the solver in its own group") {
    Tmp t("prism_qa_sess_");
    const auto pidfile = t.path / "solver.pid";
    const auto t0 = std::chrono::steady_clock::now();
    auto r = prism::detail::run_session({sibling("prism_tests").string()}, fake_prism(pidfile, 2.0));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.timed_out);
    CHECK(r.rc == -1);
    CHECK(secs < 30.0);
    const long solver = solver_pid(pidfile);
    REQUIRE(solver > 1);
    CHECK(gone(solver));
}

TEST_CASE("run_session: a SIGTERM to the runner kills the session it started") {
    // Ctrl-C while waiting: the handler main() installs kills the sessions
    Tmp t("prism_qa_sess_");
    const auto pidfile = t.path / "solver.pid";
    const pid_t p = ::fork();
    REQUIRE(p >= 0);
    if (p == 0) {
        prism::detail::install_child_cleanup();
        prism::detail::run_session({sibling("prism_tests").string()}, fake_prism(pidfile, 60.0));
        ::_exit(0);
    }
    const long solver = solver_pid(pidfile);
    REQUIRE(solver > 1);
    CHECK(::kill(static_cast<pid_t>(solver), 0) == 0);  // running before the signal
    ::kill(p, SIGTERM);
    int st = 0;
    REQUIRE(::waitpid(p, &st, 0) == p);
    CHECK(WIFSIGNALED(st));
    CHECK(WTERMSIG(st) == SIGTERM);
    CHECK(gone(solver));
}

TEST_CASE("run_session: normal exit returns stdout, stderr, the exit code, env, cwd and stdin") {
    auto r = prism::detail::run_session({"sh", "-c", "echo ok; echo err >&2; exit 3"});
    CHECK_FALSE(r.timed_out);
    CHECK_FALSE(r.failed);
    CHECK(r.rc == 3);
    CHECK(r.out == "ok\n");
    CHECK(r.err == "err\n");

    Tmp t("prism_qa_sess_");
    prism::detail::SessionOptions so;
    so.env = {{"PRISM_QA_PROBE", "v1"}, {"PATH", std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin"}};
    so.cwd = t.path;
    const std::string input = "line1\nline2\n";
    so.input = input;
    auto e = prism::detail::run_session({"sh", "-c", "echo $PRISM_QA_PROBE; pwd; cat"}, so);
    CHECK(e.rc == 0);
    auto ls = lines_of(e.out);
    REQUIRE(ls.size() == 4);
    CHECK(ls[0] == "v1");
    CHECK(fs::equivalent(ls[1], t.path));
    CHECK(ls[2] == "line1");
    CHECK(ls[3] == "line2");

    // no input: stdin is /dev/null, not the test runner's terminal
    auto n = prism::detail::run_session({"sh", "-c", "cat; echo done"});
    CHECK(n.out == "done\n");
    // a killed child reports the signal; a missing program is 127
    CHECK(prism::detail::run_session({"sh", "-c", "kill -9 $$"}).rc == -9);
    CHECK(prism::detail::run_session({"/nonexistent/prism-qa-probe"}).rc == 127);
    // a background grandchild left behind by a finished child dies with its session
    auto bg = prism::detail::run_session({"sh", "-c", "sleep 30 >/dev/null 2>&1 & echo $!"});
    const long gc = std::atol(bg.out.c_str());
    REQUIRE(gc > 1);
    CHECK(gone(gc));
}

TEST_CASE("run_session: RLIMIT_AS applies to the child") {
    prism::detail::SessionOptions so;
    so.rlimit_as_mb = 64;
    auto r = prism::detail::run_session({"sh", "-c", "ulimit -v"}, so);
    CHECK(r.rc == 0);
    CHECK(r.out == std::to_string(64 * 1024) + "\n");
}
#endif

// ---------------------------------------------------------------- triage-selfscan
TEST_CASE("triage-selfscan buckets the self-scan fixture") {
    auto sarif = prism::qa::load_json(repo() / "tests" / "data" / "selfscan_min" / "report.sarif");
    REQUIRE(sarif);
    const auto text = prism::qa::triage_text(prism::qa::triage_selfscan(*sarif));
    CHECK(has_line(text, "total FAILED: 4"));
    CHECK(has_line(text, "third_party (out of scope): 1"));
    CHECK(has_line(text, "testdata/tests (false alarm corpus): 1"));
    CHECK(has_line(text, "src/prism|gui (manual review): 1"));
    CHECK(has_line(text, ".github (false alarm LANG-LINT): 1"));
    CHECK(has_line(text, "summary: 1 / 3 / 1"));

    CHECK(prism::qa::triage_classify("third_party/z3/a.c", "FAILED", "lints") == "out_of_scope");
    CHECK(prism::qa::triage_classify("x/third_party/a.c", "FAILED", "lints") == "out_of_scope");
    CHECK(prism::qa::triage_classify("testdata_tp/a.c", "FAILED", "lints") == "false_alarm");
    CHECK(prism::qa::triage_classify("tests/a.py", "FAILED", "polyglot") == "false_alarm");
    CHECK(prism::qa::triage_classify("src/gui/a.cpp", "FAILED", "lints") == "false_alarm");
    CHECK(prism::qa::triage_classify("src/prism/a.cpp", "CRASH", "sanitize") == "real");
    CHECK(prism::qa::triage_classify("src/prism/a.cpp", "CRASH", "fuzz") == "false_alarm");
    CHECK(prism::qa::triage_classify("proofs/x.c", "FAILED", "lints") == "real");

    // ./ and \ in URIs are normalised
    nlohmann::json j = {{"runs",
                         {{{"results",
                            {{{"locations", {{{"physicalLocation", {{"artifactLocation", {{"uri", ".\\third_party\\a.c"}}}}}}}},
                              {"properties", {{"status", "FAILED"}, {"stage", "lints"}}}}}}}}}};
    auto s = prism::qa::triage_selfscan(j);
    CHECK(s.third_party == 1);  // "\" becomes "/" first, then "./" is dropped
    j["runs"][0]["results"][0]["locations"][0]["physicalLocation"]["artifactLocation"]["uri"] = "./third_party/a.c";
    s = prism::qa::triage_selfscan(j);
    CHECK(s.third_party == 1);
    CHECK(s.out_of_scope == 1);
}

#ifndef _WIN32
TEST_CASE("prism-qa triage-selfscan prints the same lines") {
    auto r = prism::detail::run_session(
        {sibling("prism-qa").string(), "triage-selfscan", (repo() / "tests" / "data" / "selfscan_min").string()});
    REQUIRE(r.rc == 0);
    CHECK(has_line(r.out, "total FAILED: 4"));
    CHECK(has_line(r.out, "summary: 1 / 3 / 1"));
    auto bad = prism::detail::run_session({sibling("prism-qa").string(), "triage-selfscan", "/nonexistent"});
    CHECK(bad.rc == 2);
    CHECK(prism::detail::run_session({sibling("prism-qa").string(), "no-such-command"}).rc == 2);
}
#endif

// ---------------------------------------------------------------- pir-vs-bmc
TEST_CASE("pir-vs-bmc buckets, matrix and hard conflicts") {
    using prism::qa::pvb_bucket;
    const std::string proved = "PROVED", cert = "PROVED-CERTIFIED", unb = "PROVED-UNBOUNDED", ass = "PROVED-ASSUMING",
                      bounded = "BOUNDED", timeout = "TIMEOUT", notrun = "NOTRUN", nh = "NEEDS-HARNESS";
    CHECK(pvb_bucket(nullptr) == "absent");
    for (auto* s : {&proved, &cert, &unb, &ass}) CHECK(pvb_bucket(s) == "PROVED*");
    CHECK(pvb_bucket(&bounded) == "BOUNDED");  // Law 2: never a PROVED*
    CHECK(pvb_bucket(&nh) == "NEEDS-HARNESS");
    CHECK(pvb_bucket(&timeout) == "UNKNOWN");
    CHECK(pvb_bucket(&notrun) == "UNKNOWN");

    auto f = [](const char* file, const char* fn, const char* st) {
        return nlohmann::json{{"file", file}, {"function", fn}, {"status", st}, {"message", "m"}};
    };
    nlohmann::json report = {
        {"stages",
         {{{"name", "bmc"},
           {"findings",
            {f("a.c", "f", "PROVED"), f("a.c", "f", "FAILED"), f("a.c", "g", "FAILED"), f("a.c", "h", "BOUNDED"),
             f("a.c", "only_bmc", "UNKNOWN"), nlohmann::json{{"file", "a.c"}, {"status", "FAILED"}}}}},
          {{"name", "pir"},
           {"findings",
            {f("a.c", "f", "PROVED-CERTIFIED"), f("a.c", "g", "PROVED"), f("a.c", "h", "PROVED"),
             f("b.c", "only_pir", "FAILED")}}}}}};
    auto r = prism::qa::pir_vs_bmc(report);
    CHECK(r.functions == 5);
    CHECK(r.bmc == 4);
    CHECK(r.pir == 4);
    CHECK(r.matrix[{"PROVED*", "PROVED*"}] == 1);  // the first finding per function is its verdict
    CHECK(r.matrix[{"FAILED", "PROVED*"}] == 1);
    CHECK(r.matrix[{"BOUNDED", "PROVED*"}] == 1);
    CHECK(r.matrix[{"UNKNOWN", "absent"}] == 1);
    CHECK(r.matrix[{"absent", "FAILED"}] == 1);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0]["function"] == "g");
    CHECK(r.conflicts[0]["bmc"] == "FAILED");
    CHECK(r.conflicts[0]["pir"] == "PROVED");
    const auto text = prism::qa::pir_vs_bmc_text(r);
    CHECK(has_line(text, "functions: 5 (bmc 4, pir 4)"));
    CHECK(has_line(text, "rows = bmc, columns = pir"));
    CHECK(has_line(text, "same bucket where both report: 1/3"));
    CHECK(has_line(text, "hard conflicts (PROVED* vs FAILED): 1"));
    CHECK(has_line(text, "  a.c::g: bmc=FAILED [m]  | pir=PROVED [m] "));
    // the matrix: right-aligned columns 15 wide, left-aligned row names
    CHECK(has_line(text, std::string(15, ' ') + "        PROVED*        BOUNDED         FAILED  NEEDS-HARNESS"
                                                "          ERROR        UNKNOWN         absent"));
    CHECK(has_line(text, "FAILED                       1              0              0              0"
                         "              0              0              0"));
    auto j = prism::qa::pir_vs_bmc_json(r);
    CHECK(j["matrix"]["FAILED|PROVED*"] == 1);
    CHECK(j["conflicts"].size() == 1);
}

// ---------------------------------------------------------------- pir-lean-check
TEST_CASE("pir-lean-check parses the checker's rows and prints the total line") {
    auto p = prism::qa::parse_checker_output(
        "agree\tf\tx\n"
        "agree-ext\tg\tx\n"
        "outside\th\tx\tcall @foo not modelled\n"
        "outside\ti\tx\tcall @bar not modelled\n"
        "MISMATCH\tj\tfile.pirl\tblock 2\n"
        "agree\tshort\n"     // fewer than three columns: not a row
        "summary: whatever\n");
    CHECK(p.counts["agree"] == 1);
    CHECK(p.counts["agree-ext"] == 1);
    CHECK(p.counts["outside"] == 2);
    CHECK(p.counts["MISMATCH"] == 1);
    CHECK(p.rows.size() == 5);
    const auto line = prism::qa::lean_total_line(p.counts);
    CHECK(line == "total: agree=1 agree-ext=1 agree-reject=0 outside=2 mismatch=1 "
                  "(functions in the proved fragment: 3)");
    // the contract the end-to-end check below (and docs/PROOFS_REFINEMENT.md) reads
    std::smatch m;
    REQUIRE(std::regex_search(line, m, std::regex(R"(total: agree=(\d+) .* mismatch=(\d+))")));
    CHECK(m[1] == "1");
    CHECK(m[2] == "1");
    CHECK(std::regex_search(line, std::regex(R"(total: agree=\d+ agree-ext=(\d+))")));
}

namespace {
std::optional<fs::path> built_checker() {
    // the checker lake builds; not built here (no Lean toolchain): NOTRUN
    auto exe = repo() / "proofs" / "refinement" / ".lake" / "build" / "bin" / "pir_lean_check";
    std::error_code ec;
    if (fs::is_regular_file(exe, ec)) return exe;
    return std::nullopt;
}
}  // namespace

#ifndef _WIN32
TEST_CASE("pir_lean_check fixtures give their expected verdicts") {
    auto checker = built_checker();
    if (!checker) {
        MESSAGE("NOTRUN: pir_lean_check not built (cd proofs/refinement && lake build)");
        return;
    }
    std::vector<fs::path> fixtures;
    for (auto& e : fs::directory_iterator(repo() / "proofs" / "refinement" / "fixtures"))
        if (e.path().extension() == ".pirl") fixtures.push_back(e.path());
    std::sort(fixtures.begin(), fixtures.end());
    REQUIRE_FALSE(fixtures.empty());
    for (const auto& f : fixtures) {
        auto want = prism::qa::read_text(fs::path(f).replace_extension(".expected"));
        while (!want.empty() && (want.back() == '\n' || want.back() == '\r' || want.back() == ' '))
            want.pop_back();
        auto r = prism::detail::run_session({checker->string(), f.string()});
        auto ls = lines_of(r.out);
        REQUIRE_FALSE(ls.empty());
        CHECK_MESSAGE(ls.back() == want, f.filename().string());
        CHECK_MESSAGE(r.rc == (want.find("mismatch=0") == std::string::npos ? 1 : 0), f.filename().string());
    }
}

TEST_CASE("pir-lean-check: tests/pir agrees with the proved translator") {
    auto checker = built_checker();
    if (!checker) {
        // the driver itself reports NOTRUN (exit 2), never a pass
        auto r = prism::detail::run_session({sibling("prism-qa").string(), "pir-lean-check",
                                             (repo() / "tests" / "pir").string(), "--checker",
                                             "/nonexistent/pir_lean_check"});
        CHECK(r.rc == 2);
        CHECK(r.out.find("NOTRUN") != std::string::npos);
        MESSAGE("NOTRUN: pir_lean_check not built (cd proofs/refinement && lake build)");
        return;
    }
    auto r = prism::detail::run_session({sibling("prism-qa").string(), "pir-lean-check",
                                         (repo() / "tests" / "pir").string(), "--bin", sibling("prism").string(),
                                         "--checker", checker->string()});
    CHECK_MESSAGE(r.rc == 0, (r.out + r.err));
    std::smatch m;
    REQUIRE_MESSAGE(std::regex_search(r.out, m, std::regex(R"(total: agree=(\d+) .* mismatch=(\d+))")), r.out);
    CHECK(std::stoi(m[2]) == 0);
    CHECK(std::stoi(m[1]) >= 30);
    // the extended fragment (freeze, calls, memory) is checked too; the pir
    // stage's time budget can leave files unexported on a loaded machine
    REQUIRE(std::regex_search(r.out, m, std::regex(R"(total: agree=\d+ agree-ext=(\d+))")));
    CHECK(std::stoi(m[1]) >= 15);
}
#endif

// ---------------------------------------------------------------- llvm-sem-vs-lli
TEST_CASE("llvm-sem-vs-lli reads .pirl records, builds input vectors and lli harnesses") {
    auto recs = prism::qa::pirl_records(
        "func add\n"
        "L params 2 x 32 y 32\n"
        "L ret 32\n"
        "L block entry\n"
        "L ret 8\n"  // a ret inside a block is an instruction, not the signature
        "end\n"
        "func bad\n"
        "L params 1 p 64\n"
        "L unsupported call @printf\n"
        "end\n"
        "func v\n"
        "L params 0\n"
        "end\n");
    REQUIRE(recs.size() == 2);
    CHECK(recs[0].name == "add");
    CHECK(recs[0].params == std::vector<int>{32, 32});
    CHECK(recs[0].ret == 32);
    CHECK(recs[1].name == "v");
    CHECK(recs[1].params.empty());
    CHECK(recs[1].ret == 0);

    std::mt19937_64 rng(20260923);
    auto vs = prism::qa::input_vectors({8, 8}, 12, rng);
    CHECK(vs.size() == 12);
    CHECK(std::is_sorted(vs.begin(), vs.end()));
    std::set<std::uint64_t> seen;
    for (auto& v : vs) {
        REQUIRE(v.size() == 2);
        CHECK(v[0] <= 255);
        CHECK(v[1] <= 255);
    }
    CHECK(std::set<std::vector<std::uint64_t>>(vs.begin(), vs.end()).size() == vs.size());  // distinct
    std::mt19937_64 again(20260923);
    CHECK(prism::qa::input_vectors({8, 8}, 12, again) == vs);  // the seed fixes the run
    std::mt19937_64 r1(7);
    for (auto& v : prism::qa::input_vectors({32}, 40, r1)) seen.insert(v[0]);
    for (std::uint64_t edge : {0ull, 1ull, 2ull, 7ull, 100ull, 0xFFFFFFFFull, 0xFFFFFFFEull, 0x80000000ull, 0x7FFFFFFFull})
        CHECK(seen.contains(edge));  // every edge value is in the pool
    std::mt19937_64 r2(1);
    CHECK(prism::qa::input_vectors({}, 12, r2) == std::vector<std::vector<std::uint64_t>>{{}});
    std::mt19937_64 r3(1);
    auto one = prism::qa::input_vectors({1}, 12, r3);
    CHECK(one.size() == 2);  // i1 has two values

    const auto h = prism::qa::lli_harness("define i32 @add(i32 %x, i32 %y) {\n  ret i32 0\n}\n", "add", {32, 32}, 32,
                                          {4294967295ull, 2});
    CHECK(h.find("%r = call i32 @add(i32 4294967295, i32 2)") != std::string::npos);
    CHECK(h.find("%z = zext i32 %r to i64") != std::string::npos);
    CHECK(h.find("declare i32 @printf(ptr, ...)") != std::string::npos);
    const auto hv = prism::qa::lli_harness("declare i32 @printf(ptr, ...)\n", "g", {}, 0, {});
    CHECK(hv.find("call void @g()") != std::string::npos);
    CHECK(hv.find("@__prism_lli_void") != std::string::npos);
    CHECK(hv.find("declare i32 @printf(ptr, ...)\n@") == std::string::npos);  // not declared twice
    CHECK(prism::qa::lli_harness("", "w", {64}, 64, {1}).find("%z = add i64 %r, 0") != std::string::npos);
    // the pir stage's markers get definitions lli can link
    const auto hm = prism::qa::lli_harness(
        "declare i8 @__prism.uninit.i8()\ndeclare void @__prism.folded(i32)\ndeclare i32 @ext(i32)\n", "f", {}, 0, {});
    CHECK(hm.find("define i8 @__prism.uninit.i8() {\n  ret i8 zeroinitializer\n}") != std::string::npos);
    CHECK(hm.find("define void @__prism.folded(i32) {\n  ret void\n}") != std::string::npos);
    CHECK(hm.find("declare i32 @ext(i32)") != std::string::npos);  // a real external stays unresolved
}

#ifndef _WIN32
TEST_CASE("llvm-sem-vs-lli: the harness runs the pir stage's IR under lli") {
    auto cfg = prism::default_config();
    auto fe = prism::pir::find_frontend(cfg);
    if (!fe.clang || !fe.opt || !fe.lli) {
        MESSAGE("NOTRUN: clang/opt/lli not found");
        return;
    }
    Tmp t("prism_qa_lli_");
    const auto src = t.path / "add.c";
    std::ofstream(src) << "unsigned char add1(unsigned char x) { return x + 1; }\n"
                          "int neg(int x) { return -x; }\n";
    std::string err;
    auto ir = prism::pir::lower_to_ir(fe, src, 60.0, err);
    REQUIRE_MESSAGE(ir, err);
    auto run = [&](const std::string& fn, int w, int ret, std::uint64_t a) {
        std::ofstream(t.path / "h.ll") << prism::qa::lli_harness(*ir, fn, {w}, ret, {a});
        prism::detail::SessionOptions so;
        so.timeout_s = 60;
        auto r = prism::detail::run_session(
            {fe.lli->string(), "--entry-function=__prism_lli_main", (t.path / "h.ll").string()}, so);
        auto ls = lines_of(r.out);
        return ls.empty() ? std::string("rc ") + std::to_string(r.rc) + " " + r.err : ls.back();
    };
    CHECK(run("add1", 8, 8, 41) == "42");
    CHECK(run("add1", 8, 8, 255) == "0");               // i8 wraps; printed zero-extended
    CHECK(run("neg", 32, 32, 1) == "4294967295");       // -1 as an unsigned 32-bit value
    // the driver without the Lean evaluator: NOTRUN, exit 2
    auto r = prism::detail::run_session({sibling("prism-qa").string(), "llvm-sem-vs-lli", src.string()});
    if (!fs::exists(repo() / "proofs" / "refinement" / ".lake" / "build" / "bin" / "llvm_eval")) {
        CHECK(r.rc == 2);
        CHECK(r.out.find("NOTRUN: missing llvm_eval") != std::string::npos);
        MESSAGE("NOTRUN: llvm_eval not built (cd proofs/refinement && lake build llvm_eval)");
    }
}
#endif

// ---------------------------------------------------------------- libc-bounds
TEST_CASE("libc-bounds splits a contract harness file into prelude and harnesses") {
    const std::string text =
        "#include \"harness.h\"\n"
        "#include \"../x/y.h\"\n"
        "\n"
        "/* strlen: returns the length.\n"
        " * second line\n"
        " */\n"
        "int strlen_true(void) {\n"
        "    char s[N];\n"
        "    return 0;\n"
        "}\n"
        "static int sign(int v) { return v < 0 ? -1 : v > 0; }\n"
        "// one line\n"
        "int one_true(void) { return 1; }\n"
        "int fixed_false(void) {\n"
        "    return 0;\n"
        "}\n";
    auto s = prism::qa::split_harness(text);
    CHECK(s.prelude ==
          "#include \"harness.h\"\n#include \"../x/y.h\"\n\n"
          "static int sign(int v) { return v < 0 ? -1 : v > 0; }\n");
    REQUIRE(s.funcs.size() == 3);
    CHECK(s.funcs[0].first == "strlen_true");
    CHECK(s.funcs[0].second ==
          "/* strlen: returns the length.\n * second line\n */\n"
          "int strlen_true(void) {\n    char s[N];\n    return 0;\n}\n");
    CHECK(s.funcs[1].first == "one_true");
    CHECK(s.funcs[1].second == "// one line\nint one_true(void) { return 1; }\n");
    CHECK(s.funcs[2].first == "fixed_false");
    auto none = prism::qa::split_harness("int x;\n");
    CHECK(none.prelude == "int x;\n");
    CHECK(none.funcs.empty());

    const auto suite = repo() / "tests" / "conformance" / "libc-models";
    auto abs = prism::qa::absolutize_includes("#include \"harness.h\"\n#include <string.h>\n", suite);
    CHECK(abs == "#include \"" + fs::weakly_canonical(suite / "harness.h").string() + "\"\n#include <string.h>\n");

    // every shipped harness file splits into _true harnesses
    int files = 0;
    for (auto& e : fs::directory_iterator(suite)) {
        if (!e.path().filename().string().ends_with("_contracts.c")) continue;
        ++files;
        auto sp = prism::qa::split_harness(prism::qa::read_text(e.path()));
        CHECK_MESSAGE(std::any_of(sp.funcs.begin(), sp.funcs.end(), [](auto& p) { return p.first.ends_with("_true"); }),
                      e.path().filename().string());
        CHECK(sp.prelude.find("#include \"harness.h\"") != std::string::npos);
    }
    CHECK(files > 0);
}

// ---------------------------------------------------------------- solver-bench
TEST_CASE("solver-bench discovers the conformance tasks it can score") {
    Tmp t("prism_qa_tasks_");
    const auto suite = t.path / "suite";
    fs::create_directories(suite / "prism" / "cat");
    fs::create_directories(suite / "sv-comp" / "bits");
    std::ofstream(suite / "prism" / "cat" / "a.c") << "int f(int x) { return x; }\n";
    std::ofstream(suite / "prism" / "cat" / "a.yml")
        << "format_version: 1\ninput_files: a.c\nlanguage: C\nproperty: no-oob\nexpected:\n  f: true\n";
    std::ofstream(suite / "prism" / "cat" / "nosrc.yml") << "input_files: missing.c\nexpected:\n  f: true\n";
    std::ofstream(suite / "prism" / "cat" / "noexp.yml") << "input_files: a.c\n";
    std::ofstream(suite / "sv-comp" / "bits" / "b.i") << "int main(void) { return 0; }\n";
    std::ofstream(suite / "sv-comp" / "bits" / "b.yml")
        << "format_version: '2.0'\n\n# comment\ninput_files: 'b.i'\n\nproperties:\n"
           "  - property_file: ../properties/unreach-call.prp\n    expected_verdict: false\n"
           "  - property_file: ../properties/no-overflow.prp\n    expected_verdict: true\n"
           "\noptions:\n  language: C\n  data_model: ILP32\n";
    std::ofstream(suite / "sv-comp" / "bits" / "c.yml")
        << "input_files: ['b.i']\nproperties:\n  - property_file: ../properties/termination.prp\n"
           "    expected_verdict: true\n  - property_file: ../properties/no-overflow.prp\n";
    auto tasks = prism::qa::discover_tasks({suite / "prism", suite / "sv-comp"}, suite);
    REQUIRE(tasks.size() == 2);
    CHECK(tasks[0].ident == "prism/cat/a.yml");
    CHECK(tasks[0].source.filename() == "a.c");
    CHECK(tasks[1].ident == "sv-comp/bits/b.yml");
    CHECK(tasks[1].source.filename() == "b.i");
    // a root outside the suite is named from its parent
    auto outside = prism::qa::discover_tasks({suite / "prism" / "cat"}, t.path / "elsewhere");
    REQUIRE(outside.size() == 1);
    CHECK(outside[0].ident == "cat/a.yml");

    // the shipped suites
    const auto real = repo() / "tests" / "conformance";
    auto all = prism::qa::discover_tasks({real / "prism", real / "sv-comp"}, real);
    CHECK(all.size() > 100);
    for (auto& task : all) CHECK(fs::exists(task.source));
}

// ---------------------------------------------------------------- assurance-check
TEST_CASE("assurance-check: the assurance package cites only existing artefacts") {
    auto r = prism::qa::assurance_scan(repo() / "docs" / "assurance", repo());
    std::string joined;
    for (auto& e : r.errors) joined += e + "\n";
    CHECK_MESSAGE(r.errors.empty(), joined);
    CHECK(r.counts.files >= 7);
    CHECK(r.counts.theorems > 20);
}

TEST_CASE("assurance-check catches missing artefacts") {
    Tmp t("prism_qa_assurance_");
    std::ofstream(t.path / "bogus.md")
        << "`thm:no_such_theorem_anywhere` `thm:proved_bounded_never_merge` "
           "`thm:Houdini.houdini_sound` `thm:Houdini.bmc_sound`\n"
           "`docs/NO_SUCH.md` `docs/VERDICTS.md#no-such-anchor` `docs/VERDICTS.md#verdict-proved`\n"
           "`tests/cpp/test_main.cpp::NoSuchTestCase` `tests/cpp/test_main.cpp::laws refuse merging proofs`\n"
           "`thm:<Name>` is a placeholder\n";
    auto r = prism::qa::assurance_scan(t.path, repo());
    std::string joined;
    for (auto& e : r.errors) joined += e + "\n";
    CHECK_MESSAGE(r.errors.size() == 5, joined);
    CHECK(joined.find("no_such_theorem_anywhere") != std::string::npos);
    CHECK(joined.find("not in namespace Houdini") != std::string::npos);
    CHECK(joined.find("docs/NO_SUCH.md") != std::string::npos);
    CHECK(joined.find("#no-such-anchor") != std::string::npos);
    CHECK(joined.find("NoSuchTestCase") != std::string::npos);
    CHECK(r.counts.theorems == 4);
    CHECK(r.counts.paths == 5);
    CHECK(r.errors[0].find("bogus.md:1: ") != std::string::npos);

    CHECK(prism::qa::is_path_ref("docs/X.md#a"));
    CHECK(prism::qa::is_path_ref("CMakeLists.txt"));
    CHECK_FALSE(prism::qa::is_path_ref("docs/*.md"));
    CHECK_FALSE(prism::qa::is_path_ref("prism PATH"));
    CHECK_FALSE(prism::qa::is_path_ref("build/prism"));

#ifndef _WIN32
    // the standalone checker (the docs CI job) agrees: exit 0 on the package, 1 here
    auto ok = prism::detail::run_session({sibling("prism_docs_check").string(), "assurance-check", "--repo",
                                          repo().string()});
    CHECK_MESSAGE(ok.rc == 0, (ok.out + ok.err));
    CHECK(ok.out.find(" 0 missing") != std::string::npos);
    auto bad = prism::detail::run_session({sibling("prism_docs_check").string(), "assurance-check", "--repo",
                                           repo().string(), "--docs", t.path.string()});
    CHECK(bad.rc == 1);
    CHECK(bad.out.find("5 missing") != std::string::npos);
    auto missing = prism::detail::run_session({sibling("prism_docs_check").string(), "assurance-check", "--docs",
                                               (t.path / "nope").string()});
    CHECK(missing.rc == 2);
#endif
}

// ---------------------------------------------------------------- docs-check
TEST_CASE("docs: slugs are GitHub's heading anchors") {
    using prism::qa::slug;
    CHECK(slug("Pipeline order is the method") == "pipeline-order-is-the-method");
    CHECK(slug("Running on untrusted code (Law 9)") == "running-on-untrusted-code-law-9");
    CHECK(slug("5. Exit codes and `--fail-on`") == "5-exit-codes-and---fail-on");
    CHECK(slug("See [the plan](PLAN.md) now") == "see-the-plan-now");
    // \w is Unicode-aware (as on GitHub): letters stay, symbols go
    CHECK(slug("Ünïcode — names × 2") == "ünïcode--names--2");
    auto a = prism::qa::anchors_of_text("# A\n## A\n```\n# not a heading\n```\n<a id=\"x-y\"></a>\n### B ##\n");
    CHECK(a == std::set<std::string>{"a", "a-1", "b", "x-y"});
}

TEST_CASE("docs: the flag parser ignores prose") {
    CHECK(prism::qa::flags_of("run perl -c on it\n  --jobs JOBS, -j JOBS  workers\n[--out DIR]") ==
          std::set<std::string>{"--jobs", "-j", "--out"});
    CHECK(prism::qa::flags_of("  --allow-\n      exec  run code") == std::set<std::string>{"--allow-exec"});
    CHECK(prism::qa::documented("--jobs", "use `--jobs N` here"));
    CHECK_FALSE(prism::qa::documented("--job", "use `--jobs N` here"));
    CHECK_FALSE(prism::qa::documented("--jobs", "use --jobs N here"));
}

#ifndef _WIN32
TEST_CASE("docs: every --help flag of prism and prism prove is in docs/USER_GUIDE.md") {
    const auto guide = prism::qa::read_text(repo() / "docs" / "USER_GUIDE.md");
    for (const auto& sub : std::vector<std::vector<std::string>>{{"--help"}, {"prove", "--help"}}) {
        std::vector<std::string> argv{sibling("prism").string()};
        argv.insert(argv.end(), sub.begin(), sub.end());
        auto r = prism::detail::run_session(argv);
        REQUIRE(r.rc == 0);
        CHECK_FALSE(prism::qa::flags_of(r.out).empty());
        auto missing = prism::qa::undocumented_flags(r.out, guide);
        std::string joined;
        for (auto& f : missing) joined += f + " ";
        CHECK_MESSAGE(missing.empty(), joined);
    }
}
#endif

TEST_CASE("docs: the user guide lists the stage order") {
    const auto guide = prism::qa::read_text(repo() / "docs" / "USER_GUIDE.md");
    std::vector<std::string> want;
    for (auto* s : prism::STAGE_ORDER)
        if (s) want.emplace_back(s);
    CHECK(prism::qa::stage_order_listed(guide) == want);
}

TEST_CASE("docs: every Markdown anchor link resolves") {
    auto bad = prism::qa::anchor_errors(repo());
    std::string joined;
    for (auto& b : bad) joined += b + "\n";
    CHECK_MESSAGE(bad.empty(), joined);
}

TEST_CASE("docs: every verdict has the VERDICTS.md anchor reports link to") {
    const auto anchors = prism::qa::anchors_of(repo() / "docs" / "VERDICTS.md");
    using namespace prism::laws;
    for (auto v : {PROVED_CERTIFIED, PROVED_UNBOUNDED, PROVED, PROVED_ASSUMING, BOUNDED, FAILED, UNKNOWN, TIMEOUT,
                   ERROR, NOFUNC, NOTRUN, NEEDS_HARNESS, CRASH, CLEAN, NOSEED, SANFAIL, HYPOTHESIS, READS}) {
        CHECK(is_status(v));
        std::string a = "verdict-";
        for (char c : v) a += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        CHECK(prism::verdict_anchor(v) == a);
        CHECK_MESSAGE(anchors.contains(a), a);
    }
}

#ifndef _WIN32
TEST_CASE("prism-qa docs-check passes on this repository") {
    auto r = prism::detail::run_session({sibling("prism-qa").string(), "docs-check", "--bin", sibling("prism").string()});
    CHECK_MESSAGE(r.rc == 0, (r.out + r.err));
    CHECK(has_line(r.out, "docs-check: 0 problems"));
}
#endif
