// External tool adapters, the AFL++ half of fuse, the process runner and the
// tool lookup (--tool, the pinned fetch_deps build, PATH). A missing tool is
// NOTRUN, a tool that answers only --help is UNKNOWN, a fake tool (doctest,
// Catch2, a shell "not found") is NOTRUN: never CLEAN, never a proof.
//
// Tools are fake executables (shell scripts in a temp dir) handed over
// through Config::tools, $PATH or $PRISM_TOOLS_DIR, so every branch runs
// against a real child process. POSIX only: the scripts need /bin/sh.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/sandbox.hpp"
#include "prism/solver.hpp"
#include "prism/stages.hpp"

#include "../../src/prism/adapters_internal.hpp"
#include "../../src/prism/proc.hpp"
#include "../../src/prism/stages/common.hpp"
#include "../../src/prism/stages/llm.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace laws = prism::laws;
namespace ad = prism::adapters_detail;
namespace sd = prism::stages_detail;

namespace {

fs::path repo_root() { return fs::path(PRISM_SOURCE_DIR); }
fs::path testdata() { return repo_root() / "testdata"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> lines_of(const fs::path& p) {
    std::vector<std::string> out;
    std::istringstream in(slurp(p));
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string xget(const prism::Finding& f, const std::string& key) {
    auto it = f.extra.find(key);
    return it == f.extra.end() ? std::string() : it->second;
}

struct TmpDir {
    fs::path dir;
    TmpDir() {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("prism_adapters_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(dir);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path put(const std::string& rel, const std::string& text) const {
        auto p = dir / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }
};

#ifndef _WIN32
// Sets (or, with nullopt, unsets) an environment variable for one scope.
struct EnvVar {
    std::string name;
    std::optional<std::string> was;
    EnvVar(std::string n, std::optional<std::string> value) : name(std::move(n)) {
        if (const char* v = std::getenv(name.c_str())) was = v;
        if (value) ::setenv(name.c_str(), value->c_str(), 1);
        else ::unsetenv(name.c_str());
    }
    ~EnvVar() {
        if (was) ::setenv(name.c_str(), was->c_str(), 1);
        else ::unsetenv(name.c_str());
    }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;
};

// An executable shell script. The body runs with a plain PATH so it can use
// mkdir/cat/printf even when the test emptied PATH for PRISM's lookups.
fs::path fake(const fs::path& dir, const std::string& name, const std::string& body) {
    fs::create_directories(dir);
    auto p = dir / name;
    {
        std::ofstream o(p, std::ios::binary);
        o << "#!/bin/sh\nPATH=/usr/bin:/bin\n" << body << "\n";
    }
    fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    return p;
}

// A script that answers the --help probe and prints `out` (exit rc) otherwise.
std::string help_then(const std::string& help, const std::string& run, int rc = 0) {
    return "case \"$1\" in --help|-h|--version|-version) printf '%s\\n' '" + help +
           "'; exit 0;; esac\n" + run + "\nexit " + std::to_string(rc);
}

// No tool anywhere: PATH is an empty dir, PRISM_TOOLS_DIR is empty.
struct NoTools {
    TmpDir t;
    fs::path bin = t.dir / "bin";
    fs::path tools = t.dir / "tools";
    EnvVar path{"PATH", (fs::create_directories(t.dir / "bin"), (t.dir / "bin").string())};
    EnvVar home{"PRISM_TOOLS_DIR", (fs::create_directories(t.dir / "tools"), (t.dir / "tools").string())};
    prism::Config cfg = [] {
        auto c = prism::default_config();
        c.root = testdata();
        c.timeout = 2;
        return c;
    }();
};

const prism::Finding& stage_row(const std::vector<prism::Finding>& out, const std::string& stage) {
    for (const auto& f : out)
        if (f.stage == stage) return f;
    FAIL("no row for stage " << stage);
    static prism::Finding none;
    return none;
}

void never_clean_or_proof(const prism::Finding& f) {
    CHECK(f.status != laws::CLEAN);
    CHECK(f.status != laws::PROVED);
    CHECK(f.status != laws::PROVED_ASSUMING);
    CHECK_FALSE(laws::is_proof(f.status));
}

const std::vector<std::string> kOptionalStages = {"klee",       "afl-fuzz", "frama-c", "infer",  "clang-tidy",
                                                  "cbmc",       "strix",    "semgrep", "spatch", "libfuzzer"};
#endif

}  // namespace

// ---------------------------------------------------------------- runner

#ifndef _WIN32
TEST_CASE("proc: one runner with stdin, split or merged output, env overlay and cwd") {
    TmpDir t;
    prism::detail::RunSpec s;
    s.argv = {"sh", "-c", "cat; echo out; echo err >&2; exit 3"};
    s.input = "from-stdin\n";
    s.timeout_s = 10;
    auto r = prism::detail::run(s);
    CHECK_FALSE(r.failed);
    CHECK_FALSE(r.timed_out);
    CHECK_FALSE(r.crashed);
    CHECK(r.rc == 3);
    CHECK(r.out == "from-stdin\nout\n");
    CHECK(r.err == "err\n");

    s.merge_stderr = true;
    s.input.clear();
    r = prism::detail::run(s);
    CHECK(r.out == "out\nerr\n");
    CHECK(r.err.empty());

    // The overlay reaches the child only; PRISM's own environment is untouched
    // (the AFL_* variables used to be setenv'd on the PRISM process).
    EnvVar gone("PRISM_TEST_OVERLAY", std::nullopt);
    prism::detail::RunSpec e;
    e.argv = {"sh", "-c", "printf '%s|%s' \"$PRISM_TEST_OVERLAY\" \"$PWD\""};
    e.env = {{"PRISM_TEST_OVERLAY", "child-only"}};
    e.cwd = t.dir;
    e.timeout_s = 10;
    auto er = prism::detail::run(e);
    CHECK(er.out == "child-only|" + fs::canonical(t.dir).string());
    CHECK(std::getenv("PRISM_TEST_OVERLAY") == nullptr);
    {
        EnvVar set("PRISM_TEST_OVERLAY", std::string("parent"));
        er = prism::detail::run(e);
        CHECK(er.out.starts_with("child-only|"));  // the overlay replaces the inherited value
        CHECK(std::string(std::getenv("PRISM_TEST_OVERLAY")) == "parent");
    }

    // A program that is not there: exec fails in the child. That is "could not
    // start" (failed, rc 127, the reason in err), not an exit code of the tool.
    prism::detail::RunSpec m;
    m.argv = {(t.dir / "no-such-program").string()};
    m.timeout_s = 5;
    auto mr = prism::detail::run(m);
    CHECK(mr.rc == 127);
    CHECK(mr.failed);
    CHECK(has(mr.err, "cannot execute " + m.argv[0]));
    // ... and so is a file without the execute bit, or a cwd that is not there.
    m.argv = {t.put("noexec.sh", "#!/bin/sh\necho hi\n").string()};
    mr = prism::detail::run(m);
    CHECK(mr.failed);
    CHECK(mr.out.empty());
    m.argv = {"sh", "-c", "echo hi"};
    m.cwd = t.dir / "no-such-dir";
    mr = prism::detail::run(m);
    CHECK(mr.failed);
    CHECK(has(mr.err, "cannot enter"));
    // A tool that itself exits 127 started: that is its exit code, not failed.
    m.cwd.clear();
    m.argv = {"sh", "-c", "exit 127"};
    mr = prism::detail::run(m);
    CHECK(mr.rc == 127);
    CHECK_FALSE(mr.failed);

    // A child that exits without reading a large stdin: the write gets EPIPE
    // (SIGPIPE is blocked for it), PRISM survives, the exit code is kept.
    prism::detail::RunSpec q;
    q.argv = {"sh", "-c", "exit 4"};
    q.input.assign(4u << 20, 'x');
    q.timeout_s = 10;
    auto qr = prism::detail::run(q);
    CHECK(qr.rc == 4);
    CHECK_FALSE(qr.failed);

    // Law 8: no runner starts a child with a check disabled.
    CHECK_THROWS_AS(prism::detail::run_process({"x", "--no-bounds-check"}, 5), std::runtime_error);
    prism::detail::RunSpec l8;
    l8.argv = {"sh", "-c", "true", "--no-pointer-check"};
    CHECK_THROWS_AS(prism::detail::run(l8), std::runtime_error);
    CHECK_THROWS_AS(sd::run_argv({"sh", "--no-div-by-zero-check"}, "", 5), std::runtime_error);

    // A signal death is a crash, a timeout is not.
    prism::detail::RunSpec k;
    k.argv = {"sh", "-c", "kill -SEGV $$"};
    k.timeout_s = 5;
    auto kr = prism::detail::run(k);
    CHECK(kr.crashed);
    CHECK(kr.rc == -11);
    k.argv = {"sh", "-c", "sleep 5"};
    k.timeout_s = 0.3;
    kr = prism::detail::run(k);
    CHECK(kr.timed_out);
    CHECK_FALSE(kr.crashed);

    // A child that exits while its background grandchild holds the pipe
    // returns at once, not after the grandchild.
    const auto t0 = std::chrono::steady_clock::now();
    prism::detail::RunSpec g;
    g.argv = {"sh", "-c", "sleep 3 & echo started"};
    g.timeout_s = 20;
    auto gr = prism::detail::run(g);
    CHECK(gr.rc == 0);
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 2.5);

    // The stage and adapter wrappers are the same runner.
    auto sr = sd::run_argv({"sh", "-c", "cat; echo e >&2"}, "in\n", 10);
    CHECK(sr.out == "in\n");
    CHECK(sr.err == "e\n");
    auto pr = prism::detail::run_process({"sh", "-c", "echo a; echo b >&2; pwd"}, 10, t.dir);
    CHECK(pr.text == "a\nb\n" + fs::canonical(t.dir).string() + "\n");
}

TEST_CASE("sandbox_run: compile then run through the runner; exec held without --allow-exec") {
    auto off = sd::sandbox_run("int main(void){return 0;}", 8, false);
    CHECK(off["ok"] == false);
    CHECK(off["error"] == "exec-disabled");
    if (!sd::which_cc()) {
        MESSAGE("no C compiler: sandbox_run compile/run half not exercised");
        return;
    }
    auto ok = sd::sandbox_run("#include <stdio.h>\nint main(void){puts(\"hi\");return 0;}\n", 8, true);
    CHECK(ok["ok"] == true);
    CHECK(ok["stdout"] == "hi\n");
    CHECK(ok["code"] == 0);
    CHECK(ok.contains("sandbox"));
    auto ex = sd::sandbox_run("int main(void){return 3;}\n", 8, true);
    CHECK(ex["ok"] == false);
    CHECK(ex["error"] == "exit");
    CHECK(ex["code"] == 3);
    auto bad = sd::sandbox_run("int main(void){ this is not C }\n", 8, true);
    CHECK(bad["ok"] == false);
    CHECK(bad["error"] == "compile");
}
#endif

TEST_CASE("llm_httpish: connection errors are a missing backend, not a failed repair") {
    CHECK(sd::llm_httpish("Connection aborted."));
    CHECK(sd::llm_httpish("connection refused"));
    CHECK(sd::llm_httpish("[Errno 111] Connection refused"));
    CHECK(sd::llm_httpish("llama.cpp/Ollama not reachable"));
    CHECK_FALSE(sd::llm_httpish("candidate did not compile"));
    CHECK_FALSE(sd::llm_httpish(""));
}

TEST_CASE("execute_cex replays concretely (not a proof); rlef_repair without a model is NOTRUN") {
    auto src = testdata() / "div_param.c";
    prism::FunctionInfo fn;
    for (auto& f : prism::extract_functions(src, src.string()))
        if (f.name == "div_param") fn = f;
    REQUIRE(fn.name == "div_param");
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = std::string(laws::FAILED);
    fail.file = fn.file;
    fail.function = fn.name;
    fail.line = fn.line;
    fail.cls = "INT-DIV-ZERO";
    fail.message = "div by zero";
    fail.counterexample = "x=1,y=0";
    auto cfg = prism::default_config();
    cfg.llm = false;
    auto out = prism::execute_cex({fail}, {fn}, cfg);
    REQUIRE_FALSE(out.empty());
    CHECK(xget(out[0], "oracle") == "concrete-replay");
    CHECK((out[0].status == laws::CRASH || out[0].status == laws::CLEAN));
    for (const auto& f : out) CHECK_FALSE(laws::is_proof(f.status));

    cfg.gguf = "/nonexistent/prism-missing.gguf";
    cfg.ollama_host.clear();
    cfg.llama_server = "http://127.0.0.1:1";
    auto rep = prism::rlef_repair(fail, cfg);
    REQUIRE_FALSE(rep.empty());
    CHECK(rep[0].stage == "repair");
    CHECK(rep[0].status == laws::NOTRUN);
    CHECK(has(rep[0].message, sd::LLM_UNAVAILABLE_MSG));
}

TEST_CASE("codeql: adapter removed; MANIFEST records why; no optional-tool row") {
    auto manifest = slurp(repo_root() / "third_party" / "MANIFEST.toml");
    CHECK(has(manifest, "name = \"codeql\""));
    CHECK(has(manifest, "restricted commercial use"));
    CHECK(has(manifest, "adapter was removed"));
    for (const char* file : {"adapters.cpp", "config.cpp", "taxonomy.cpp", "polyglot.cpp"}) {
        auto raw = slurp(repo_root() / "src" / "prism" / file);
        std::string code;
        for (std::size_t i = 0; i < raw.size();) {
            if (raw[i] == '/' && i + 1 < raw.size() && raw[i + 1] == '/') {
                while (i < raw.size() && raw[i] != '\n') ++i;
                continue;
            }
            code.push_back(raw[i++]);
        }
        CHECK(lower(code).find("codeql") == std::string::npos);
    }
    auto out = prism::run_optional_tools({}, prism::default_config());
    for (const auto& f : out) CHECK(f.stage != "codeql");
}

TEST_CASE("harness_for_parsefail maps what the BMC cannot model and nothing else") {
    auto t = prism::harness_for_parsefail("throw unencoded", "bitvector BMC");
    REQUIRE(t.has_value());
    CHECK(has(*t, "exception model"));
    auto u = prism::harness_for_parsefail("UNENCODED: bit-field", "bitvector BMC");
    REQUIRE(u.has_value());
    CHECK(has(*u, "not a proof"));
    // An unmapped parse failure is an ERROR for the caller, not a generic stub.
    CHECK_FALSE(prism::harness_for_parsefail("unexpected token ')'", "bitvector BMC").has_value());
}

// ---------------------------------------------------------------- lookup

#ifndef _WIN32
TEST_CASE("config: which needs an executable file, like shutil.which") {
    TmpDir t;
    EnvVar path("PATH", (t.dir / "bin").string());
    t.put("bin/plain", "not executable\n");
    fs::create_directories(t.dir / "bin" / "adir");
    prism::Config cfg;
    CHECK_FALSE(cfg.which({"plain"}).has_value());
    CHECK_FALSE(cfg.which({"adir"}).has_value());
    fs::permissions(t.dir / "bin" / "plain", fs::perms::owner_all);
    auto hit = cfg.which({"missing", "plain"});
    REQUIRE(hit.has_value());
    CHECK(hit->filename() == "plain");
}

TEST_CASE("config: --tool beats the pinned build, which beats PATH; PATH is last") {
    NoTools nt;
    auto commit = prism::pinned_commit("klee");
    REQUIRE(commit.has_value());
    auto vendored = fake(nt.tools / "klee" / *commit / "bin", "klee", "exit 0");
    auto onpath = fake(nt.bin, "klee", "exit 0");
    auto explicit_ = fake(nt.t.dir / "mine", "klee", "exit 0");
    auto cfg = nt.cfg;
    cfg.tools["klee"] = explicit_;
    CHECK(cfg.which_adapter("klee", {"klee"}) == explicit_);
    cfg.tools.clear();
    CHECK(cfg.which_adapter("klee", {"klee"}) == vendored);
    // A build of any other commit is not the pinned tool.
    fs::rename(nt.tools / "klee" / *commit, nt.tools / "klee" / std::string(40, '0'));
    CHECK(cfg.which_adapter("klee", {"klee"}) == onpath);
    fs::remove(onpath);
    CHECK_FALSE(cfg.which_adapter("klee", {"klee"}).has_value());
    // --tool NAME=~/... is expanded like Path.expanduser.
    EnvVar home("HOME", (nt.t.dir / "mine").string());
    cfg.tools["klee"] = "~/klee";
    CHECK(cfg.which_adapter("klee", {"klee"}) == nt.t.dir / "mine" / "klee");
    CHECK(prism::expand_user("~") == nt.t.dir / "mine");
    CHECK(prism::expand_user("~x/y") == fs::path("~x/y"));
    CHECK(prism::expand_user("/abs/p") == fs::path("/abs/p"));
}

#ifndef _WIN32
TEST_CASE("config: source trees under PRISM_TOOLS_DIR are never adapters") {
    NoTools nt;
    const std::vector<std::pair<std::string, std::string>> stages{
        {"cppcheck", "cppcheck"}, {"semgrep", "semgrep"}, {"infer", "infer"},
        {"frama-c", "frama-c"},   {"klee", "klee"},       {"esbmc", "esbmc"},
        {"dafny", "dafny"},       {"cbmc", "cbmc"},       {"spatch", "spatch"},
        {"strix", "strix"},
    };
    for (auto [stage, bin] : stages) {
        const std::string comp = (stage == "spatch") ? "coccinelle" : stage;
        auto commit = prism::pinned_commit(comp);
        REQUIRE(commit.has_value());
        auto root = nt.tools / comp / *commit;
        fs::create_directories(root / "bin");
        fs::create_directories(root / "src");
        {
            std::ofstream(root / "bin" / bin) << "int main(void){return 0;}\n";
        }
        auto naked = root / "src" / bin;
        {
            std::ofstream(naked) << "// source\n";
        }
        fs::permissions(naked, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
        CAPTURE(stage);
        CHECK_FALSE(nt.cfg.which_adapter(stage, {bin}).has_value());
    }
}

TEST_CASE("config: clang-tidy is never taken from PRISM_TOOLS_DIR") {
    NoTools nt;
    auto dir = nt.tools / "clang-tidy" / std::string(40, 'a') / "bin";
    fake(dir, "clang-tidy", "exit 0");
    CHECK_FALSE(nt.cfg.which_adapter("clang-tidy", {"clang-tidy"}).has_value());
}
#endif

TEST_CASE("config: every pinned external stage has a prism-deps hint (bitwuzla too)") {
    // [[component]] rows of kind "external": each of their stages maps to the
    // component, so adapter_install names its prism-deps command.
    std::ifstream in(repo_root() / "third_party" / "MANIFEST.toml");
    std::string line, name, kind;
    int checked = 0;
    while (std::getline(in, line)) {
        if (line.ends_with('\r')) line.pop_back();  // autocrlf checkouts
        if (line.starts_with("[[")) name.clear(), kind.clear();
        auto val = [&](const char* key) -> std::optional<std::string> {
            std::string k = std::string(key) + " = ";
            if (!line.starts_with(k)) return std::nullopt;
            return line.substr(k.size());
        };
        if (auto v = val("name")) name = v->substr(1, v->size() - 2);
        if (auto v = val("kind")) kind = v->substr(1, v->size() - 2);
        auto st = val("stages");
        if (!st || kind != "external") continue;
        std::string rest = *st;
        for (auto q = rest.find('"'); q != std::string::npos; q = rest.find('"', q)) {
            auto e = rest.find('"', q + 1);
            auto stage = rest.substr(q + 1, e - q - 1);
            q = e + 1;
            auto hint = prism::adapter_install(stage);
            CHECK_MESSAGE(has(hint, "prism-deps tool " + name), stage << " -> " << hint);
            CHECK_MESSAGE(prism::pinned_commit(name).has_value(), name);
            ++checked;
        }
    }
    CHECK(checked >= 15);
    CHECK(has(prism::adapter_install("bitwuzla"), "prism-deps tool bitwuzla"));
    auto esbmc = prism::adapter_install("esbmc");
    CHECK(has(esbmc, "prism-deps tool esbmc"));
    CHECK(has(esbmc, "third_party/MANIFEST.toml"));
    CHECK_FALSE(has(esbmc, "SOURCES.md"));
    CHECK_FALSE(has(esbmc, "apt install"));
}

TEST_CASE("solver find_tool: --tool, then the pinned build under PRISM_TOOLS_DIR, never another commit") {
    NoTools nt;
    auto pin = prism::pinned_commit("cadical");
    REQUIRE(pin.has_value());
    auto pinned = fake(nt.tools / "cadical" / *pin / "bin", "cadical", "exit 0");
    auto other = fake(nt.tools / "cadical" / std::string(40, 'a') / "bin", "cadical", "exit 0");
    // The other build is newer: the old search took the newest of any commit.
    fs::last_write_time(other, fs::last_write_time(pinned) + std::chrono::hours(1));
    prism::solver::SolveOptions o;
    auto hit = prism::solver::find_tool("cadical", o);
    REQUIRE(hit.has_value());
    CHECK(hit->path == pinned);
    CHECK(hit->version == *pin);
    // Only the other commit: not the pinned tool, and nothing on PATH.
    fs::remove_all(nt.tools / "cadical" / *pin);
    CHECK_FALSE(prism::solver::find_tool("cadical", o).has_value());
    // Explicit tool_dirs (tests, bench) keep taking the newest build.
    o.tool_dirs = {nt.tools.string()};
    REQUIRE(prism::solver::find_tool("cadical", o).has_value());
    CHECK(prism::solver::find_tool("cadical", o)->path == other);
    o.tool_dirs.clear();
    // --tool cadical=PATH wins over everything.
    auto mine = fake(nt.t.dir / "mine", "cadical-x", "exit 0");
    o.tool_paths["cadical"] = mine.string();
    hit = prism::solver::find_tool("cadical", o);
    REQUIRE(hit.has_value());
    CHECK(hit->path == mine);
    CHECK(hit->version == "config");
    // Law 9: a tools dir inside the scanned tree is not searched.
    o.tool_paths.clear();
    fake(nt.tools / "cake_lpr" / *prism::pinned_commit("cake_lpr") / "bin", "cake_lpr", "exit 0");
    CHECK(prism::solver::find_tool("cake_lpr", o).has_value());
    o.refuse_tools_under = nt.t.dir.string();
    CHECK_FALSE(prism::solver::find_tool("cake_lpr", o).has_value());
    o.refuse_tools_under = testdata().string();
    CHECK(prism::solver::find_tool("cake_lpr", o).has_value());
}

// ---------------------------------------------------------------- optional tools

TEST_CASE("optional tools: every missing tool is NOTRUN with its install hint") {
    NoTools nt;
    for (const auto& paths : {std::vector<fs::path>{}, std::vector<fs::path>{testdata()},
                              std::vector<fs::path>{testdata() / "abs_ok.c", testdata() / "div_param.c"}}) {
        auto out = prism::run_optional_tools(paths, nt.cfg);
        REQUIRE_FALSE(out.empty());
        std::vector<std::string> stages;
        for (const auto& f : out) stages.push_back(f.stage);
        CHECK(stages == kOptionalStages);
        for (const auto& f : out) {
            CAPTURE(f.stage);
            CHECK(f.status == laws::NOTRUN);
            never_clean_or_proof(f);
            auto install = xget(f, "install");
            CHECK(has(install, "third_party/MANIFEST.toml"));
            if (f.stage != "clang-tidy" && f.stage != "libfuzzer")
                CHECK(has(install, "prism-deps tool"));
            if (f.stage == "libfuzzer") {
                CHECK(f.message == "clang not on PATH");
            } else {
                CHECK(install == prism::adapter_install(f.stage));
                CHECK(has(f.message, "not found"));
            }
        }
        CHECK(has(xget(stage_row(out, "afl-fuzz"), "install"), "prism-deps tool aflplusplus"));
        CHECK(has(xget(stage_row(out, "infer"), "install"), "prism-deps tool infer"));
        CHECK(has(xget(stage_row(out, "frama-c"), "install"), "prism-deps tool frama-c"));
        // frama-c (the EVA adapter) is not the wp stage.
        for (const auto& f : out) CHECK(f.stage != "wp");
    }
}

TEST_CASE("optional tools: wp is its own stage, not the missing frama-c") {
    auto src = testdata() / "acsl_abs.c";
    auto wp = prism::run_wp(prism::extract_functions(src, src.filename().string()), 8);
    REQUIRE_FALSE(wp.empty());
    for (const auto& f : wp) CHECK(f.stage == "wp");
    CHECK(wp[0].status != laws::NOTRUN);
}

TEST_CASE("optional tools: a --help answer is UNKNOWN, never a code verdict") {
    NoTools nt;
    auto cfg = nt.cfg;
    cfg.tools["klee"] = fake(nt.t.dir / "t", "klee", help_then("KLEE --help", ""));
    cfg.tools["afl-fuzz"] = fake(nt.t.dir / "t", "afl-fuzz", "exit 0");  // rc 0, no output
    for (const auto& paths : {std::vector<fs::path>{}, std::vector<fs::path>{testdata() / "abs_ok.c"}}) {
        auto out = prism::run_optional_tools(paths, cfg);
        auto afl = stage_row(out, "afl-fuzz");
        CHECK(afl.status == laws::UNKNOWN);
        CHECK(afl.status != laws::NOTRUN);
        CHECK(afl.status != laws::ERROR);
        never_clean_or_proof(afl);
        CHECK(has(lower(afl.message), "not a code verdict"));
        CHECK(xget(afl, "help_exit") == "0");
        for (const auto& f : out) {
            if (f.stage == "klee" || f.stage == "afl-fuzz" || f.stage == "libfuzzer") continue;
            CHECK_MESSAGE(f.status == laws::NOTRUN, f.stage);
        }
        if (paths.empty()) {
            auto klee = stage_row(out, "klee");
            CHECK(klee.status == laws::UNKNOWN);
            never_clean_or_proof(klee);
        }
    }
}

TEST_CASE("optional tools: a binary that does not answer --help is NOTRUN, never ERROR") {
    NoTools nt;
    auto cfg = nt.cfg;
    cfg.timeout = 1;  // each probe flag gets at most the check timeout
    // Cannot start (no execute bit): the C++ runner reports exit 127, so the
    // row is "did not answer" (the Python engine's exception path).
    cfg.tools["klee"] = nt.t.put("t/klee", "#!/bin/sh\nexit 0\n");
    cfg.tools["infer"] = fake(nt.t.dir / "t", "infer", "exit 3");  // silent, rc 3
    cfg.tools["semgrep"] = fake(nt.t.dir / "t", "semgrep", "sleep 30");  // every flag times out
    cfg.tools["frama-c"] = fake(nt.t.dir / "t", "frama-c",
                                "echo 'sh: 1: /opt/Code Analysis/.prism/tools/frama-c/bin/frama-c: not found' >&2; "
                                "exit 127");
    cfg.tools["cbmc"] = fake(nt.t.dir / "t", "cbmc", "echo 'Catch2 v3.5.0'; exit 0");
    const auto t0 = std::chrono::steady_clock::now();
    auto out = prism::run_optional_tools({}, cfg);
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 20);
    for (const char* stage : {"klee", "infer", "semgrep", "frama-c", "cbmc"}) {
        CAPTURE(stage);
        auto f = stage_row(out, stage);
        CHECK(f.status == laws::NOTRUN);
        CHECK(f.status != laws::ERROR);
        CHECK(f.status != laws::UNKNOWN);
        never_clean_or_proof(f);
        CHECK(has(f.message, "did not answer --help"));
        CHECK_FALSE(has(lower(f.message), "present"));
        CHECK(xget(f, "exe") == cfg.tools[stage].string());
        CHECK(xget(f, "install") == prism::adapter_install(stage));
    }
    // Catch2 answering --help as Frama-C is not Frama-C.
    cfg.tools["frama-c"] = fake(nt.t.dir / "t", "frama-c", "echo 'Catch2 v3.5.0'; exit 0");
    auto frama = stage_row(prism::run_optional_tools({}, cfg), "frama-c");
    CHECK(frama.status == laws::NOTRUN);
    CHECK(has(frama.message, "did not answer --help"));
}

TEST_CASE("adapters: doctest / Catch2 / shell not-found text is a missing tool") {
    CHECK(ad::probe_looks_missing("[doctest] doctest version is \"2.4.11\"\n--stack-trace", 0));
    CHECK(ad::probe_looks_missing("Unknown option: --timeout\n", 1));
    CHECK(ad::probe_looks_missing("", 127));
    CHECK_FALSE(ad::probe_looks_missing("", 0));
    CHECK_FALSE(ad::probe_looks_missing("usage: cbmc [options] file.c", 1));
    CHECK(ad::is_fake_adapter("Catch2 v3.5.0\n"));
    CHECK(ad::is_fake_adapter("[doctest] doctest version is 2.4.11\n"));
    CHECK_FALSE(ad::is_fake_adapter("CBMC 5.95.1\n"));
    CHECK(ad::tool_unusable("Catch2 v3.5.0\n", 0));
    CHECK(ad::tool_unusable("", 126));
    CHECK(ad::tool_unusable("", 127));
    CHECK(ad::tool_unusable("sh: 1: cbmc: not found\n", 1));
    CHECK(ad::tool_unusable("'cbmc' is not recognized as an internal or external command\n", 1));
    CHECK(ad::tool_unusable("cannot execute binary file\n", 126));
    CHECK_FALSE(ad::tool_unusable("VERIFICATION SUCCESSFUL\n", 0));
}

TEST_CASE("cbmc: never passes a check-disabling flag; a fake cbmc is NOTRUN") {
    // Law 8: every adapter command line is refused if it disables a check.
    CHECK_THROWS_WITH_AS(ad::refuse_disabled_checks({"cbmc", "a.c", "--no-bounds-check"}),
                         "refusing to disable a check: --no-bounds-check", std::runtime_error);
    CHECK_NOTHROW(ad::refuse_disabled_checks({"cbmc", "a.c", "--bounds-check"}));
    NoTools nt;
    auto args = nt.t.dir / "argv.txt";
    auto cbmc = fake(nt.t.dir / "t", "cbmc", "echo \"$@\" >> '" + args.string() +
                                                 "'\necho 'VERIFICATION SUCCESSFUL'");
    auto paths = std::vector<fs::path>{testdata() / "abs_ok.c"};
    auto out = ad::run_cbmc(cbmc.string(), paths, nt.cfg);
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == laws::BOUNDED);  // unwind-limited: BOUNDED, never PROVED (Law 2)
    CHECK_FALSE(laws::is_proof(out[0].status));
    auto argv = slurp(args);
    CHECK(has(argv, "--unwind"));
    CHECK_FALSE(has(argv, "--no-"));

    auto doctest = fake(nt.t.dir / "t", "cbmc2",
                        "echo 'Unknown option: --timeout' >&2; echo ' --stack-trace' >&2; "
                        "echo '[doctest] doctest version is 2.4.11' >&2; exit 1");
    auto catch2 = fake(nt.t.dir / "t", "cbmc3", "echo 'Catch2 v3.5.0'; echo 'Unknown option: --unwind'");
    for (const auto& exe : {doctest, catch2}) {
        auto r = ad::run_cbmc(exe.string(), paths, nt.cfg);
        REQUIRE_FALSE(r.empty());
        CHECK(r[0].status == laws::NOTRUN);
        CHECK(r[0].status != laws::ERROR);
        CHECK(r[0].status != laws::BOUNDED);
        never_clean_or_proof(r[0]);
        CHECK(has(r[0].message, "not CBMC"));
        CHECK(xget(r[0], "install") == prism::adapter_install("cbmc"));
    }
}

TEST_CASE("semgrep: a match is FAILED; no results or silent output is UNKNOWN") {
    NoTools nt;
    auto paths = std::vector<fs::path>{testdata() / "abs_ok.c"};
    auto cfg = nt.cfg;
    const std::string hit =
        R"({"results":[{"check_id":"c.lang.security","path":"test.c","start":{"line":3},"extra":{"message":"use-after-free"}}]})";
    cfg.tools["semgrep"] = fake(nt.t.dir / "t", "semgrep", help_then("semgrep --help", "echo '" + hit + "'"));
    auto sg = stage_row(prism::run_optional_tools(paths, cfg), "semgrep");
    CHECK(sg.status == laws::FAILED);
    CHECK(sg.cls == "c.lang.security");
    CHECK(sg.line == 3);
    CHECK(sg.message == "use-after-free");

    for (const char* body : {"echo '{\"results\":[]}'", "printf '  \\n\\t\\n'", "true"}) {
        CAPTURE(body);
        cfg.tools["semgrep"] = fake(nt.t.dir / "t", "semgrep", help_then("semgrep --help", body));
        auto r = stage_row(prism::run_optional_tools(paths, cfg), "semgrep");
        CHECK(r.status == laws::UNKNOWN);  // whitespace-only output used to be ERROR
        never_clean_or_proof(r);
        CHECK(has(lower(r.message), "no matches"));
    }
    CHECK(ad::extract_json_object("") == "{}");
    CHECK(ad::extract_json_object(" \n\t ") == "{}");
    CHECK(ad::extract_json_object("warn\n{\"results\":[]}\ntrailer") == "{\"results\":[]}");
}

namespace {
// Frama-C fake: answers the probe, then prints `eva_out` on -eva runs.
std::string frama_fake(const std::string& eva_out, int rc = 0) {
    return help_then("Frama-C EVA plugin",
                     "case \"$*\" in *-eva*) printf '" + eva_out + "\\n'; exit " + std::to_string(rc) +
                         ";; esac\nexit 0");
}

std::vector<std::string> finding_key(const std::vector<prism::Finding>& fs) {
    std::vector<std::string> k;
    for (const auto& f : fs)
        k.push_back(f.stage + "|" + f.status + "|" + f.file + "|" + f.cls + "|" + f.message);
    return k;
}
}  // namespace

TEST_CASE("frama-c EVA: zero alarms is UNKNOWN; warnings and alarms are FAILED; doctest is NOTRUN") {
    NoTools nt;
    auto cfg = nt.cfg;
    auto c = testdata() / "abs_ok.c";
    auto exe = fake(nt.t.dir / "t", "frama-c", frama_fake("[eva] 0 alarms emitted"));
    cfg.tools["frama-c"] = exe;
    auto zero = ad::run_frama_c(exe.string(), {c}, cfg);
    REQUIRE(zero.size() == 1);
    CHECK(zero[0].status == laws::UNKNOWN);
    CHECK(has(zero[0].message, "not a proof"));
    never_clean_or_proof(zero[0]);

    auto no_c = ad::run_frama_c(exe.string(), {testdata() / "unit.cpp"}, cfg);
    REQUIRE(no_c.size() == 1);
    CHECK(no_c[0].status == laws::UNKNOWN);
    CHECK(has(no_c[0].message, "no .c files"));
    never_clean_or_proof(no_c[0]);

    exe = fake(nt.t.dir / "t", "frama-c-warn", frama_fake("warning: signed overflow", 1));
    auto warn = ad::run_frama_c(exe.string(), {c}, cfg);
    REQUIRE_FALSE(warn.empty());
    CHECK(warn[0].status == laws::FAILED);
    CHECK(warn[0].cls == "FUNC-CONTRACT");
    never_clean_or_proof(warn[0]);

    exe = fake(nt.t.dir / "t", "frama-c-alarm", frama_fake("[eva] 1 alarm emitted"));
    auto one = ad::run_frama_c(exe.string(), {c}, cfg);
    REQUIRE_FALSE(one.empty());
    CHECK(one[0].status == laws::FAILED);
    never_clean_or_proof(one[0]);

    exe = fake(nt.t.dir / "t", "frama-c-ten", frama_fake("[eva] 10 alarms emitted"));
    auto ten = ad::run_frama_c(exe.string(), {c}, cfg);
    REQUIRE_FALSE(ten.empty());
    CHECK(ten[0].status == laws::FAILED);
    CHECK(ten[0].status != laws::UNKNOWN);
    never_clean_or_proof(ten[0]);

    exe = fake(nt.t.dir / "t", "frama-c-uninit",
               frama_fake("[eva] alarm: accessing uninitialized left-value"));
    auto uninit = ad::run_frama_c(exe.string(), {c}, cfg);
    REQUIRE_FALSE(uninit.empty());
    CHECK(uninit[0].status == laws::FAILED);
    CHECK(uninit[0].cls == "UNINIT-READ");
    never_clean_or_proof(uninit[0]);

    cfg.timeout = 1;
    exe = fake(nt.t.dir / "t", "frama-c-sleep",
               help_then("Frama-C", "case \"$*\" in *-eva*) sleep 30;; esac\nexit 0"));
    auto to = ad::run_frama_c(exe.string(), {c}, cfg);
    REQUIRE(to.size() == 1);
    CHECK(to[0].status == laws::TIMEOUT);
    never_clean_or_proof(to[0]);

    exe = fake(nt.t.dir / "t", "frama-c-doctest",
               help_then("Frama-C",
                         "case \"$*\" in *-eva*) "
                         "printf '[doctest] doctest version is 2.4.11\\nUnknown option: --timeout\\n' >&2; "
                         "exit 1;; esac\nexit 0"));
    auto imp = ad::run_frama_c(exe.string(), {c}, cfg);
    REQUIRE(imp.size() == 1);
    CHECK(imp[0].status == laws::NOTRUN);
    CHECK(has(lower(imp[0].message), "not frama-c"));
    CHECK(imp[0].status != laws::UNKNOWN);
    CHECK(imp[0].status != laws::ERROR);
    never_clean_or_proof(imp[0]);
}

TEST_CASE("infer: missing is NOTRUN; no issues is UNKNOWN, not a proof") {
    NoTools nt;
    auto paths = std::vector<fs::path>{testdata() / "abs_ok.c"};
    auto cfg = nt.cfg;
    cfg.tools["infer"] = fake(nt.t.dir / "t", "infer", help_then("infer --help", "echo 'No issues found'"));
    // infer present but no compiler for `infer run -- cc`: NOTRUN.
    auto nocc = stage_row(prism::run_optional_tools(paths, cfg), "infer");
    CHECK(nocc.status == laws::NOTRUN);
    CHECK(xget(nocc, "install") == "install gcc or clang");
    fake(nt.bin, "gcc", "exit 0");
    auto infer = stage_row(prism::run_optional_tools(paths, cfg), "infer");
    CHECK(infer.status == laws::UNKNOWN);
    never_clean_or_proof(infer);
    CHECK(has(lower(infer.message), "no issues"));
    CHECK(has(lower(infer.message), "not a proof"));
}

TEST_CASE("cocci: the shipped rules are found from any cwd") {
    prism::Config cfg;
    auto names = [](const std::vector<fs::path>& rules) {
        std::set<std::string> n;
        for (const auto& r : rules) n.insert(r.filename().string());
        return n;
    };
    CHECK(names(ad::cocci_rules({}, cfg)).contains("getenv_null.cocci"));
    CHECK(names(ad::cocci_rules({testdata() / "getenv_null.c"}, cfg)).contains("getenv_null.cocci"));
    // From a cwd with no prism/cocci above it, the rules next to the binary.
    auto here = fs::current_path();
    fs::current_path(fs::path("/"));
    auto rules = names(ad::cocci_rules({}, cfg));
    fs::current_path(here);
    CHECK(rules.contains("getenv_null.cocci"));
    CHECK(rules.contains("realloc_self.cocci"));
    CHECK(rules.size() >= 8);
    for (const char* need :
         {"memcpy_self.cocci", "realloc_self.cocci", "shift_bit31.cocci", "getenv_null.cocci",
          "strcpy_self.cocci", "sprintf_unbounded.cocci", "strcat_self.cocci", "strncpy_self.cocci"})
        CHECK(rules.contains(need));
}

TEST_CASE("klee: .err dumps and KLEE ERROR are FAILED; a quiet run is UNKNOWN") {
    NoTools nt;
    auto cfile = testdata() / "abs_ok.c";
    fake(nt.bin, "clang",
         "OUT=\nwhile [ $# -gt 0 ]; do case \"$1\" in -o) OUT=\"$2\"; shift 2;; *) shift;; esac; "
         "done\n[ -n \"$OUT\" ] && printf 'BC' > \"$OUT\"\nexit 0\n");
    auto cfg = nt.cfg;
    cfg.allow_exec = true;
    auto mk_klee = [&](const std::string& body) {
        return fake(nt.t.dir / "t", "klee", help_then("KLEE --help", body));
    };
    cfg.tools["klee"] = mk_klee(
        "mkdir -p klee-out/klee-last\n"
        "echo x > klee-out/klee-last/test000001.ptr.err\n"
        "echo 'KLEE: done'\n");
    auto out = ad::run_klee(cfg.tools["klee"].string(), {cfile}, cfg);
    REQUIRE_FALSE(out.empty());
    CHECK(out[0].status == laws::FAILED);
    never_clean_or_proof(out[0]);

    cfg.tools["klee"] = mk_klee("echo 'KLEE: ERROR: invalid pointer' >&2; exit 1");
    out = ad::run_klee(cfg.tools["klee"].string(), {cfile}, cfg);
    REQUIRE_FALSE(out.empty());
    CHECK(out[0].status == laws::FAILED);
    never_clean_or_proof(out[0]);

    cfg.tools["klee"] = mk_klee("echo 'KLEE: done: generated tests = 2'");
    out = ad::run_klee(cfg.tools["klee"].string(), {cfile}, cfg);
    REQUIRE_FALSE(out.empty());
    CHECK(out[0].status == laws::UNKNOWN);
    never_clean_or_proof(out[0]);
    CHECK(has(lower(out[0].message), "not a proof"));

    cfg.tools["klee"] = mk_klee(
        "echo '[doctest] doctest version is 2.4.11' >&2; echo 'Unknown option: --max-time' >&2; exit 1");
    out = ad::run_klee(cfg.tools["klee"].string(), {cfile}, cfg);
    REQUIRE_FALSE(out.empty());
    CHECK(out[0].status == laws::NOTRUN);
    never_clean_or_proof(out[0]);
    CHECK(has(lower(out[0].message), "not klee"));
}

TEST_CASE("clang-tidy: no TUs is UNKNOWN; silence is not a proof; warnings are FAILED") {
    NoTools nt;
    auto cfg = nt.cfg;
    auto cfile = testdata() / "abs_ok.c";
    auto log = nt.t.dir / "argv.log";
    auto tidy = fake(nt.t.dir / "t", "clang-tidy",
                     "echo \"$@\" >> '" + log.string() +
                         "'\n"
                         "case \"$1\" in *.cpp|*.cc|*.cxx) exit 0;; esac\n"
                         "printf '%s:3:5: warning: use after free [clang-analyzer-unix.Malloc]\\n' \"$1\"\n");
    auto none = ad::run_clang_tidy(tidy.string(), {}, cfg);
    REQUIRE_FALSE(none.empty());
    CHECK(none[0].status == laws::UNKNOWN);
    never_clean_or_proof(none[0]);
    CHECK(has(none[0].message, "no C/C++ translation units"));

    auto hdr = ad::run_clang_tidy(tidy.string(), {fs::path("n.h")}, cfg);
    REQUIRE_FALSE(hdr.empty());
    CHECK(hdr[0].status == laws::UNKNOWN);

    auto quiet = fake(nt.t.dir / "t", "clang-tidy-q", "true");
    auto silent = ad::run_clang_tidy(quiet.string(), {cfile}, cfg);
    REQUIRE_FALSE(silent.empty());
    CHECK(silent[0].status == laws::UNKNOWN);
    CHECK(has(silent[0].message, "no diagnostics (not a proof)"));
    never_clean_or_proof(silent[0]);

    auto warned = ad::run_clang_tidy(tidy.string(), {cfile}, cfg);
    REQUIRE_FALSE(warned.empty());
    CHECK(warned[0].status == laws::FAILED);
    CHECK(has(warned[0].message, ": warning:"));
    never_clean_or_proof(warned[0]);

    auto cxx = nt.t.put("unit.cpp", "int f(void){return 0;}\n");
    log = nt.t.dir / "cxx.log";
    auto tidycxx = fake(nt.t.dir / "t", "clang-tidy-cxx", "echo \"$@\" >> '" + log.string() + "'\nexit 0\n");
    ad::run_clang_tidy(tidycxx.string(), {cxx}, cfg);
    auto args = slurp(log);
    CHECK(has(args, "-std=c++11"));
    CHECK(!has(args, "-std=c11"));

    auto doctest = fake(nt.t.dir / "t", "clang-tidy-fake",
                        "printf '[doctest] doctest version is 2.4.11\\nUnknown option: --timeout\\n'; exit 1");
    auto fake_out = ad::run_clang_tidy(doctest.string(), {cfile}, cfg);
    REQUIRE_FALSE(fake_out.empty());
    CHECK(fake_out[0].status == laws::NOTRUN);
    CHECK(has(lower(fake_out[0].message), "not clang-tidy"));
    never_clean_or_proof(fake_out[0]);
}

TEST_CASE("strix: help-only is UNKNOWN; counterexample FAILED; doctest is NOTRUN") {
    NoTools nt;
    auto cfg = nt.cfg;
    auto src = nt.t.put("unit.c", "int main(void){return 0;}\n");
    auto strix = fake(nt.t.dir / "t", "strix", help_then("strix --help", ""));
    auto probe = ad::run_strix(strix.string(), {src}, cfg);
    REQUIRE_FALSE(probe.empty());
    CHECK(probe[0].status == laws::UNKNOWN);
    CHECK(has(lower(probe[0].message), "not a code verdict"));
    never_clean_or_proof(probe[0]);

    auto spec = nt.t.put("bad.ltl", "G p\n");
    cfg.tools["strix"] = fake(nt.t.dir / "t", "strix-ce",
                              help_then("strix --help", "printf 'counterexample found\\n'; exit 1"));
    auto fail = stage_row(prism::run_optional_tools({spec}, cfg), "strix");
    CHECK(fail.status == laws::FAILED);
    CHECK(fail.cls == "strix");
    never_clean_or_proof(fail);

    cfg.tools["strix"] = fake(nt.t.dir / "t", "strix-ok", help_then("strix --help", "printf 'REALIZABLE\\n'"));
    auto unk = stage_row(prism::run_optional_tools({spec}, cfg), "strix");
    CHECK(unk.status == laws::UNKNOWN);
    CHECK(has(lower(unk.message), "not a proof"));
    never_clean_or_proof(unk);

    cfg.tools["strix"] = fake(nt.t.dir / "t", "strix-fake",
                              help_then("strix --help", "printf '[doctest] doctest version is 2.4.11\\n'; exit 0"));
    auto nr = stage_row(prism::run_optional_tools({spec}, cfg), "strix");
    CHECK(nr.status == laws::NOTRUN);
    CHECK(has(lower(nr.message), "not strix"));
    never_clean_or_proof(nr);
}

TEST_CASE("warnings: fake compilers parse output, dedupe, and never emit proof") {
    NoTools nt;
    auto cfg = nt.cfg;
    cfg.root = nt.t.dir;
    auto unit = nt.t.put("planted.c", "int main(void){return 0;}\n");
    const std::string warn = "planted.c:3:5: warning: overflow [-Woverflow]\n";
    fake(nt.bin, "gcc", "printf '" + warn + "' >&2\nexit 0\n");
    auto clang = nt.bin / "clang";
    fs::copy_file(nt.bin / "gcc", clang, fs::copy_options::overwrite_existing);
    auto out = prism::run_compiler({unit}, cfg);
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == laws::FAILED);
    CHECK(out[0].cls == "compiler-warning");
    CHECK(out[0].line == 3);
    never_clean_or_proof(out[0]);

    fake(nt.bin, "gcc", "echo 'fatal: cannot exec cc1' >&2\nexit 1\n");
    fs::copy_file(nt.bin / "gcc", clang, fs::copy_options::overwrite_existing);
    auto bad = prism::run_compiler({unit}, cfg);
    REQUIRE_FALSE(bad.empty());
    CHECK(bad[0].status == laws::FAILED);
    CHECK(bad[0].cls == "compiler-error");
    never_clean_or_proof(bad[0]);

    auto log = nt.t.dir / "cc.log";
    fake(nt.bin, "gcc", "echo \"$@\" >> '" + log.string() + "'\nexit 0\n");
    fs::remove(clang);
    fake(nt.bin, "clang", "echo \"$@\" >> '" + log.string() + "'\nexit 0\n");
    auto cxx = nt.t.put("unit.cpp", "int f(void){return 0;}\n");
    prism::run_compiler({cxx}, cfg);
    CHECK(has(slurp(log), "-std=c++11"));
    CHECK(!has(slurp(log), "-std=c11"));
}

TEST_CASE("warnings: gcc and clang agree once; same path is not run twice") {
    NoTools nt;
    auto cfg = nt.cfg;
    cfg.root = nt.t.dir;
    auto unit = nt.t.put("planted.c", "int main(void){return 0;}\n");
    const std::string warn = "planted.c:3:5: warning: overflow [-Woverflow]\n";
    fake(nt.bin, "gcc", "printf '" + warn + "' >&2\nexit 0\n");
    fake(nt.bin, "clang", "printf '" + warn + "' >&2\nexit 0\n");
    auto both = prism::run_compiler({unit}, cfg);
    REQUIRE(both.size() == 1);
    CHECK(has(xget(both[0], "compilers"), "gcc"));
    CHECK(has(xget(both[0], "compilers"), "clang"));
    never_clean_or_proof(both[0]);

    auto log = nt.t.dir / "cc-once.log";
    fake(nt.bin, "gcc", "echo \"$@\" >> '" + log.string() + "'\nexit 0\n");
    fs::remove(nt.bin / "clang");
    fs::copy_file(nt.bin / "gcc", nt.bin / "clang", fs::copy_options::overwrite_existing);
    prism::run_compiler({unit}, cfg);
    CHECK(lines_of(log).size() == 1);
}

TEST_CASE("warnings: jobs=1 and jobs=4 yield the same ordered findings") {
    NoTools nt;
    std::vector<fs::path> units;
    for (int i = 0; i < 8; ++i)
        units.push_back(nt.t.put("u" + std::to_string(i) + ".c", "int main(void){return 0;}\n"));
    const std::string script =
        "f=''\n"
        "for a in \"$@\"; do case \"$a\" in *.c) f=\"$a\";; esac; done\n"
        "idx=$(basename \"$f\" .c | sed 's/^u//')\n"
        "sleep $(awk -v i=\"$idx\" 'BEGIN{printf \"%.3f\", 0.02*(8-i)}')\n"
        "case \"$idx\" in 2) sleep 35;; 5) exit 127;; 6) printf '%s:6:1: warning: w [-W]\\n' \"$f\" >&2; "
        "exit 1;; esac\n"
        "printf '/src/common.h:3:1: warning: shared header\\n' >&2\n"
        "exit 0\n";
    fake(nt.bin, "gcc", script);
    auto run = [&](int jobs) {
        auto cfg = nt.cfg;
        cfg.root = nt.t.dir;
        cfg.jobs = jobs;
        return prism::run_compiler(units, cfg);
    };
    auto serial = run(1);
    auto parallel = run(4);
    CHECK(finding_key(serial) == finding_key(parallel));
    CHECK(std::count_if(serial.begin(), serial.end(),
                        [](const prism::Finding& f) { return f.status == laws::TIMEOUT; }) >= 1);
    CHECK(std::count_if(serial.begin(), serial.end(),
                        [](const prism::Finding& f) { return f.status == laws::NOTRUN; }) >= 1);
}

TEST_CASE("clang-tidy: jobs=1 and jobs=4 yield the same ordered findings") {
    NoTools nt;
    std::vector<fs::path> units;
    for (int i = 0; i < 6; ++i)
        units.push_back(nt.t.put("u" + std::to_string(i) + ".c", "int main(void){return 0;}\n"));
    auto tidy = fake(nt.t.dir / "t", "clang-tidy",
                     "f=\"$2\"\n"
                     "idx=$(basename \"$f\" .c | sed 's/^u//')\n"
                     "sleep $(awk -v i=\"$idx\" 'BEGIN{printf \"%.3f\", 0.002*(6-i)}')\n"
                     "case \"$idx\" in 1) sleep 35;; esac\n"
                     "case $((idx % 2)) in 0) exit 0;; esac\n"
                     "printf '%s:1:1: warning: w%s\\n' \"$f\" \"$idx\"\n"
                     "exit $([ \"$idx\" = \"4\" ] && echo 3 || echo 0)\n");
    auto run = [&](int jobs) {
        auto cfg = nt.cfg;
        cfg.jobs = jobs;
        return ad::run_clang_tidy(tidy.string(), units, cfg);
    };
    auto serial = run(1);
    auto parallel = run(4);
    CHECK(finding_key(serial) == finding_key(parallel));
    REQUIRE_FALSE(serial.empty());
    CHECK(serial[0].status == laws::TIMEOUT);
}

TEST_CASE("spatch: a match is FAILED; silence is UNKNOWN; held script rules stay written down") {
    NoTools nt;
    auto c = testdata() / "abs_ok.c";
    auto cfg = nt.cfg;
    cfg.tools["spatch"] =
        fake(nt.t.dir / "t", "spatch", help_then("spatch --help", "echo '" + c.string() + ":12: realloc(p, n)'"));
    auto out = prism::run_optional_tools({c}, cfg);
    std::set<std::string> classes;
    for (const auto& f : out)
        if (f.stage == "spatch" && f.status == laws::FAILED) {
            classes.insert(f.cls);
            CHECK(f.line == 12);
        }
    CHECK(classes.contains("realloc_self"));

    cfg.tools["spatch"] = fake(nt.t.dir / "t", "spatch", help_then("spatch --help", ""));
    auto quiet = stage_row(prism::run_optional_tools({c}, cfg), "spatch");
    CHECK(quiet.status == laws::UNKNOWN);
    never_clean_or_proof(quiet);
    CHECK(has(lower(quiet.message), "no matches"));

    // "No rules apply" with rc 1 is silence, not a broken spatch.
    auto norules = fake(nt.t.dir / "t", "spatch-nr",
                        "echo 'No rules apply. Perhaps your semantic patch' >&2; exit 1");
    auto nr = ad::run_spatch(norules.string(), {c}, nt.cfg);
    REQUIRE_FALSE(nr.empty());
    for (const auto& f : nr) CHECK(f.status != laws::ERROR);
    CHECK(nr[0].status == laws::UNKNOWN);
    CHECK(has(nr[0].message, "not a proof"));

    // Law 7: a rule with a script block is held without --allow-exec, and the
    // held rows survive whatever the other rules found (they were dropped).
    TmpDir tree;
    auto src = tree.put("a.c", "int f(void) { return 0; }\n");
    tree.put("scripted.cocci", "@r@\nexpression E;\n@@\nf(E)\n\n@script:python@\nx << r.E;\n@@\nprint(x)\n");
    auto held = ad::run_spatch(norules.string(), {src}, nt.cfg);
    REQUIRE(held.size() == 2);
    CHECK(held[0].status == laws::NOTRUN);
    CHECK(xget(held[0], "rule") == "scripted.cocci");
    CHECK(xget(held[0], "reason") == prism::sandbox::EXEC_REASON);
    CHECK(held[1].status == laws::UNKNOWN);
    CHECK(ad::cocci_has_script(tree.dir / "scripted.cocci"));
    CHECK_FALSE(ad::cocci_has_script(repo_root() / "prism" / "cocci" / "getenv_null.cocci"));
    auto trusted = nt.cfg;
    trusted.allow_exec = true;
    auto ran = ad::run_spatch(norules.string(), {src}, trusted);
    REQUIRE(ran.size() == 1);
    CHECK(ran[0].status == laws::UNKNOWN);
}

TEST_CASE("libfuzzer probe: missing clang or no -fsanitize=fuzzer is NOTRUN; support is UNKNOWN") {
    NoTools nt;
    auto lf = stage_row(prism::run_optional_tools({}, nt.cfg), "libfuzzer");
    CHECK(lf.status == laws::NOTRUN);
    never_clean_or_proof(lf);
    CHECK(has(xget(lf, "install"), "third_party/MANIFEST.toml"));

    fake(nt.bin, "clang", "echo \"error: unsupported argument '-fsanitize=fuzzer'\" >&2; exit 1");
    lf = stage_row(prism::run_optional_tools({}, nt.cfg), "libfuzzer");
    CHECK(lf.status == laws::NOTRUN);
    never_clean_or_proof(lf);
    CHECK(has(lower(lf.message), "libfuzzer"));

    for (const char* err :
         {"error: unknown argument: '-fsanitize=fuzzer'",
          "clang: error: unrecognized command line option '-fsanitize=fuzzer'"}) {
        CAPTURE(err);
        fake(nt.bin, "clang", "echo \"" + std::string(err) + "\" >&2; exit 1");
        lf = stage_row(prism::run_optional_tools({}, nt.cfg), "libfuzzer");
        CHECK(lf.status == laws::NOTRUN);
        never_clean_or_proof(lf);
    }

    fake(nt.bin, "clang",
         "echo \"error: unsupported argument '-fsanitize=fuzzer'\" >&2; exit 1\n"
         "echo \"clang: error: linker command failed\" >&2; exit 1");
    lf = stage_row(prism::run_optional_tools({}, nt.cfg), "libfuzzer");
    CHECK(lf.status == laws::NOTRUN);
    never_clean_or_proof(lf);

    fake(nt.bin, "clang", "exit 0");
    lf = stage_row(prism::run_optional_tools({}, nt.cfg), "libfuzzer");
    CHECK(lf.status == laws::UNKNOWN);
    never_clean_or_proof(lf);
    CHECK(has(lower(lf.message), "not a code verdict"));
    CHECK(lf.file.empty());
}

TEST_CASE("libfuzzer_probe: keyword reject and successful stub compile are honest") {
    NoTools nt;
    auto probe = [&](const std::string& body) {
        fake(nt.bin, "clang", body);
        return prism::libfuzzer_probe(nt.cfg);
    };
    auto unsupported = probe("echo \"error: unsupported argument '-fsanitize=fuzzer'\" >&2; exit 1");
    CHECK(unsupported.status == laws::NOTRUN);
    never_clean_or_proof(unsupported);

    auto unknown = probe("echo \"error: unknown argument: '-fsanitize=fuzzer'\" >&2; exit 1");
    CHECK(unknown.status == laws::NOTRUN);
    never_clean_or_proof(unknown);

    auto ok = probe("case \"$*\" in *-fsanitize=fuzzer*) exit 0;; esac\nexit 0");
    CHECK(ok.status == laws::UNKNOWN);
    never_clean_or_proof(ok);
    CHECK(has(lower(ok.message), "not a code verdict"));
}

// ---------------------------------------------------------------- esbmc / dafny / cppcheck

TEST_CASE("esbmc: the verdict line maps to PROVED, BOUNDED, FAILED, UNKNOWN or ERROR") {
    NoTools nt;
    auto args = nt.t.dir / "argv.txt";
    auto paths = std::vector<fs::path>{testdata() / "abs_ok.c"};
    struct Case {
        const char* out;
        std::string_view status;
    };
    const Case cases[] = {
        {"VERIFICATION SUCCESSFUL", laws::PROVED},
        // An unwinding assertion in the output: the bound was the limit.
        {"UNWINDING ASSERTION loop 0\\nVERIFICATION SUCCESSFUL", laws::BOUNDED},
        // PROVED_UNBOUNDED needs --k-induction on the command line, which PRISM does not pass.
        {"Solving with k-induction INDUCTION step\\nVERIFICATION SUCCESSFUL", laws::PROVED},
        {"VERIFICATION FAILED", laws::FAILED},
        {"VERIFICATION UNKNOWN", laws::UNKNOWN},
        {"Segmentation fault", laws::ERROR},
    };
    for (const auto& c : cases) {
        CAPTURE(c.out);
        auto cfg = nt.cfg;
        cfg.tools["esbmc"] = fake(nt.t.dir / "t", "esbmc",
                                  "echo \"$@\" > '" + args.string() + "'\nprintf '" + c.out + "\\n'");
        auto out = prism::run_esbmc(paths, cfg);
        REQUIRE(out.size() == 1);
        CHECK(out[0].status == c.status);
        CHECK(out[0].strength == laws::STRENGTH_PROVES);
        CHECK_FALSE(xget(out[0], "tool_sha").empty());
        auto argv = slurp(args);
        CHECK(has(argv, "--overflow-check"));
        CHECK(has(argv, "--memory-leak-check"));
        CHECK_FALSE(has(argv, "--no-"));
    }
    auto cfg = nt.cfg;
    cfg.tools["esbmc"] = fake(nt.t.dir / "t", "esbmc", "echo '[doctest] doctest version is \"2.4.11\"'");
    auto fake_out = prism::run_esbmc(paths, cfg);
    REQUIRE(fake_out.size() == 1);
    CHECK(fake_out[0].status == laws::NOTRUN);
    CHECK(xget(fake_out[0], "install") == prism::adapter_install("esbmc"));
    auto missing = prism::run_esbmc(paths, nt.cfg);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].status == laws::NOTRUN);
    CHECK(has(missing[0].message, "not found"));
}

TEST_CASE("dafny: exit 0 is PROVED, anything else FAILED; fake or missing is NOTRUN") {
    NoTools nt;
    TmpDir t;
    auto dfy = t.put("m.dfy", "method M() ensures true {}\n");
    auto cfg = nt.cfg;
    cfg.tools["dafny"] = fake(nt.t.dir / "t", "dafny", "echo 'Dafny program verifier finished with 1 verified, 0 errors'");
    auto ok = prism::run_dafny({dfy}, cfg);
    REQUIRE(ok.size() == 1);
    CHECK(ok[0].status == laws::PROVED);
    CHECK(ok[0].cls == "FUNC-CONTRACT");
    cfg.tools["dafny"] = fake(nt.t.dir / "t", "dafny", "echo 'm.dfy(1,0): Error: postcondition might not hold'; exit 4");
    auto bad = prism::run_dafny({dfy}, cfg);
    REQUIRE(bad.size() == 1);
    CHECK(bad[0].status == laws::FAILED);
    cfg.tools["dafny"] = fake(nt.t.dir / "t", "dafny", "echo 'Catch2 v3.5.0'");
    auto fk = prism::run_dafny({dfy}, cfg);
    REQUIRE(fk.size() == 1);
    CHECK(fk[0].status == laws::NOTRUN);
    auto none = prism::run_dafny({testdata() / "abs_ok.c"}, cfg);
    REQUIRE(none.size() == 1);
    CHECK(none[0].status == laws::NOTRUN);
    CHECK(has(none[0].message, "no .dfy files"));
    auto missing = prism::run_dafny({dfy}, nt.cfg);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].status == laws::NOTRUN);
}

TEST_CASE("cppcheck: XML errors are FAILED; silence UNKNOWN; odd exit ERROR; cannot start NOTRUN") {
    NoTools nt;
    auto paths = std::vector<fs::path>{testdata() / "abs_ok.c"};
    auto cfg = nt.cfg;
    cfg.tools["cppcheck"] = fake(
        nt.t.dir / "t", "cppcheck",
        "echo '<error id=\"nullPointer\" severity=\"error\" msg=\"Null &apos;p&apos;\">"
        "<location file=\"abs_ok.c\" line=\"3\"/>' >&2");
    auto out = prism::run_cppcheck(paths, cfg);
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == laws::FAILED);
    CHECK(out[0].cls == "nullPointer");
    CHECK(out[0].message == "Null 'p'");
    CHECK(out[0].line == 3);
    cfg.tools["cppcheck"] = fake(nt.t.dir / "t", "cppcheck", "exit 0");
    out = prism::run_cppcheck(paths, cfg);
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == laws::UNKNOWN);
    cfg.tools["cppcheck"] = fake(nt.t.dir / "t", "cppcheck", "echo boom; exit 3");
    out = prism::run_cppcheck(paths, cfg);
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == laws::ERROR);
    // An exe that cannot be executed did not start: the runner reports the
    // exec failure, so the row says so instead of blaming the tool.
    const auto noexec = nt.t.put("t/cppcheck-noexec", "#!/bin/sh\n");  // no execute bit
    cfg.tools["cppcheck"] = noexec;
    out = prism::run_cppcheck(paths, cfg);
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == laws::NOTRUN);
    CHECK(out[0].message == "cppcheck unusable: failed to start " + noexec.string());
    CHECK(xget(out[0], "install") == prism::adapter_install("cppcheck"));
    // Something that starts but answers like a test binary is not cppcheck.
    const auto impostor = fake(nt.t.dir / "t", "cppcheck", "echo '[doctest] doctest version is 2.4'; exit 0");
    cfg.tools["cppcheck"] = impostor;
    out = prism::run_cppcheck(paths, cfg);
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == laws::NOTRUN);
    CHECK(out[0].message == "cppcheck at " + impostor.string() + " is not cppcheck (not a proof)");
    CHECK(xget(out[0], "install") == prism::adapter_install("cppcheck"));
}

// ---------------------------------------------------------------- AFL++

namespace {
prism::FunctionInfo scalar_fn(const fs::path& file) {
    prism::FunctionInfo f;
    f.file = file.string();
    f.name = "inc";
    f.kind = "SCALAR";
    f.line = 1;
    f.signature = "int inc(int x)";
    f.params = {{"int", "x"}};
    f.body = "return x + 1;";
    return f;
}

prism::FunctionInfo saturate() {
    auto src = testdata() / "saturate.c";
    for (auto& f : prism::extract_functions(src, src.string()))
        if (f.name == "saturate") return f;
    FAIL("saturate not found");
    return {};
}

// A fake afl-fuzz: logs its environment and cwd next to -o DIR, and plants a
// crash there when `crash` is set.
std::string fake_afl_body(bool crash, const fs::path& calls = {}) {
    std::string b =
        "out=''\nwhile [ $# -gt 0 ]; do case \"$1\" in -o) out=\"$2\"; shift;; esac; shift; done\n"
        "printf '%s %s %s' \"$AFL_NO_UI\" \"$AFL_SKIP_CPUFREQ\" \"$AFL_NO_AFFINITY\" > \"$out/../env.txt\"\n"
        "pwd > \"$out/../cwd.txt\"\n";
    if (!calls.empty()) b += "echo call >> '" + calls.string() + "'\n";
    if (crash) b += "mkdir -p \"$out/crashes\"\nprintf '\\001\\002\\003\\004' > \"$out/crashes/id:000000\"\n";
    return b;
}
}  // namespace

TEST_CASE("afl: afl-fuzz is found through --tool, the pinned build, then PATH") {
    NoTools nt;
    CHECK_FALSE(sd::afl_fuzz_which(nt.cfg).has_value());
    auto pin = prism::pinned_commit("aflplusplus");
    REQUIRE(pin.has_value());
    auto vendored = fake(nt.tools / "aflplusplus" / *pin / "bin", "afl-fuzz", "exit 0");
    CHECK(sd::afl_fuzz_which(nt.cfg) == vendored);  // before: PATH only
    auto mine = fake(nt.t.dir / "mine", "afl-fuzz", "exit 0");
    auto cfg = nt.cfg;
    cfg.tools["afl-fuzz"] = mine;
    CHECK(sd::afl_fuzz_which(cfg) == mine);
    // Law 9: a pinned build inside the scanned tree is refused without --allow-exec.
    auto tree = nt.cfg;
    tree.root = nt.t.dir;
    CHECK_FALSE(sd::afl_fuzz_which(tree).has_value());
    tree.allow_exec = true;
    CHECK(sd::afl_fuzz_which(tree) == vendored);
    fs::remove_all(nt.tools / "aflplusplus");
    auto onpath = fake(nt.bin, "afl-fuzz", "exit 0");
    CHECK(sd::afl_fuzz_which(nt.cfg) == onpath);
}

TEST_CASE("afl: missing AFL or a POINTER function runs nothing; no compiler is NOTRUN") {
    NoTools nt;
    prism::sandbox::Policy allow(true);
    TmpDir t;
    auto src = t.put("planted.c", "int inc(int x) { return x + 1; }\n");
    auto calls = t.dir / "calls.txt";
    CHECK_FALSE(sd::run_afl_fuzz(scalar_fn(src), src, nt.cfg, 1.0).has_value());
    auto cfg = nt.cfg;
    cfg.tools["afl-fuzz"] = fake(t.dir / "t", "afl-fuzz", fake_afl_body(false, calls));
    auto ptr = scalar_fn(src);
    ptr.kind = "POINTER";
    ptr.params = {{"int *", "p"}};
    CHECK_FALSE(sd::run_afl_fuzz(ptr, src, cfg, 1.0).has_value());
    CHECK_FALSE(fs::exists(calls));
    // AFL present, no gcc/clang on PATH: NOTRUN, never CLEAN, never engine=afl.
    auto rec = sd::run_afl_fuzz(scalar_fn(src), src, cfg, 1.0);
    REQUIRE(rec.has_value());
    CHECK(rec->status == laws::NOTRUN);
    CHECK(rec->status != laws::ERROR);
    never_clean_or_proof(*rec);
    CHECK_FALSE(rec->extra.contains("engine"));
    CHECK(xget(*rec, "install") == "install gcc or clang");
    CHECK(has(lower(rec->message), "compiler"));
    CHECK_FALSE(fs::exists(calls));
    auto [ok, err] = sd::compile_afl_harness(t.dir / "h.c", t.dir / "h.exe");
    CHECK_FALSE(ok);
    CHECK(err == "no C compiler on PATH");
}

TEST_CASE("afl: without --allow-exec the harness is held (Law 9)") {
    NoTools nt;
    prism::sandbox::Policy deny(false);
    TmpDir t;
    auto src = t.put("planted.c", "int inc(int x) { return x + 1; }\n");
    auto cfg = nt.cfg;
    cfg.tools["afl-fuzz"] = fake(t.dir / "t", "afl-fuzz", fake_afl_body(true));
    auto rec = sd::run_afl_fuzz(scalar_fn(src), src, cfg, 1.0);
    REQUIRE(rec.has_value());
    CHECK(rec->status == laws::NOTRUN);
    CHECK(xget(*rec, "reason") == prism::sandbox::EXEC_REASON);
    CHECK_FALSE(rec->extra.contains("engine"));
    CHECK(rec->function == std::optional<std::string>("inc"));
}

TEST_CASE("afl: a planted crash is CRASH, a quiet run CLEAN (not a proof); env and cwd are the child's") {
    if (!prism::Config{}.which({"gcc", "clang"})) {
        MESSAGE("no C compiler: the AFL harness cannot be built here");
        return;
    }
    prism::sandbox::Policy allow(true);
    EnvVar no_ui("AFL_NO_UI", std::nullopt);
    EnvVar no_cpu("AFL_SKIP_CPUFREQ", std::nullopt);
    EnvVar no_aff("AFL_NO_AFFINITY", std::nullopt);
    TmpDir t;
    auto src = t.put("planted.c", "int inc(int x) { return x + 1; }\n");
    auto cfg = prism::default_config();
    cfg.root = t.dir;
    for (bool crash : {true, false}) {
        CAPTURE(crash);
        auto work = t.dir / (crash ? "w_crash" : "w_clean");
        cfg.tools["afl-fuzz"] = fake(t.dir / "t", crash ? "afl-crash" : "afl-clean", fake_afl_body(crash));
        auto rec = sd::run_afl_fuzz(scalar_fn(src), src, cfg, 1.0, work);
        REQUIRE(rec.has_value());
        CHECK(rec->status != laws::PROVED);
        CHECK(xget(*rec, "engine") == "afl");
        CHECK(xget(*rec, "sandbox") == prism::sandbox::kind());
        CHECK_FALSE(laws::is_proof(rec->status));
        if (crash) {
            CHECK(rec->status == laws::CRASH);
            CHECK(rec->cls == "AFL-CRASH");
            CHECK(rec->counterexample == std::optional<std::string>("01020304"));
            CHECK(xget(*rec, "afl_crashes") == "1");
        } else {
            CHECK(rec->status == laws::CLEAN);
            CHECK(has(lower(rec->message), "not a proof"));
        }
        // AFL_* defaults reach afl-fuzz, in the work dir, and never PRISM itself.
        CHECK(slurp(work / "env.txt") == "1 1 1");
        auto cwd = slurp(work / "cwd.txt");
        if (!cwd.empty() && cwd.back() == '\n') cwd.pop_back();
        CHECK(fs::equivalent(fs::path(cwd), work));
        CHECK(std::getenv("AFL_NO_UI") == nullptr);
        CHECK(std::getenv("AFL_SKIP_CPUFREQ") == nullptr);
    }
    // A value the user exported wins over the default.
    EnvVar user("AFL_NO_UI", std::string("0"));
    auto work = t.dir / "w_user";
    auto rec = sd::run_afl_fuzz(scalar_fn(src), src, cfg, 1.0, work);
    REQUIRE(rec.has_value());
    CHECK(slurp(work / "env.txt") == "0 1 1");
}

TEST_CASE("fuse: PRISM_AFL opt-in; AFL on hand but not opted in; opted in but missing") {
    const std::string real_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    NoTools nt;
    EnvVar lf("PRISM_LIBFUZZER", std::nullopt);
    auto fn = saturate();
    TmpDir t;
    auto calls = t.dir / "calls.txt";
    auto with_afl = nt.cfg;
    with_afl.tools["afl-fuzz"] = fake(t.dir / "t", "afl-fuzz", fake_afl_body(false, calls));
    {
        // AFL available, PRISM_AFL unset: the concrete fuzz loop still runs and
        // the row says AFL could have run; AFL itself does not.
        EnvVar afl("PRISM_AFL", std::nullopt);
        auto recs = prism::run_fuse({fn}, {}, testdata(), 0.2, 4, false, with_afl);
        REQUIRE_FALSE(recs.empty());
        CHECK(recs[0].status == laws::CLEAN);
        CHECK(recs[0].function == std::optional<std::string>("saturate"));
        CHECK_FALSE(xget(recs[0], "fuzz_iters").empty());
        CHECK(xget(recs[0], "afl_available") == "true");
        CHECK_FALSE(recs[0].extra.contains("engine"));
        CHECK_FALSE(fs::exists(calls));
    }
    {
        // PRISM_AFL=1 and afl-fuzz missing: NOTRUN for the AFL half, the row
        // stays the concrete CLEAN (not a proof), never engine=afl.
        EnvVar afl("PRISM_AFL", std::string("1"));
        auto recs = prism::run_fuse({fn}, {}, testdata(), 0.2, 4, false, nt.cfg);
        REQUIRE_FALSE(recs.empty());
        CHECK(recs[0].status == laws::CLEAN);
        CHECK(xget(recs[0], "afl") == "NOTRUN");
        CHECK(xget(recs[0], "install") == prism::adapter_install("afl-fuzz"));
        CHECK(xget(recs[0], "engine") != "afl");
        CHECK(has(recs[0].message, "not a proof"));
        CHECK_FALSE(laws::is_proof(recs[0].status));
    }
    EnvVar path("PATH", real_path);  // the harness needs the real gcc/clang
    if (!prism::Config{}.which({"gcc", "clang"})) {
        MESSAGE("no C compiler: the PRISM_AFL=1 run with afl-fuzz present is not exercised");
        return;
    }
    {
        // PRISM_AFL=1 with afl-fuzz present and --allow-exec: AFL runs once.
        EnvVar afl("PRISM_AFL", std::string("1"));
        prism::sandbox::Policy allow(true);
        auto recs = prism::run_fuse({fn}, {}, testdata(), 0.2, 4, false, with_afl);
        REQUIRE_FALSE(recs.empty());
        CHECK(recs[0].status == laws::CLEAN);
        CHECK(xget(recs[0], "engine") == "afl");
        CHECK_FALSE(recs[0].extra.contains("afl_available"));
        CHECK(slurp(calls) == "call\n");
        // The binary half of fuzz_function ran after the concrete oracle.
        CHECK(xget(recs[0], "sandbox") == prism::sandbox::kind());
    }
}

// ---------------------------------------------------------------- fuzz_function oracles

namespace {
// A fake C compiler on PATH: logs its first argument (the sanitizer flag set,
// or -O0 for the bare try) to `log`, rejects the flag sets in `reject`, and
// writes `exe_body` as the "compiled" program at the -o path (exe_body
// empty: every build fails).
fs::path fake_cc(const fs::path& bin, const fs::path& log, const std::vector<std::string>& reject,
                 const std::string& exe_body) {
    std::string b = "echo \"$1\" >> '" + log.string() + "'\n";
    for (const auto& r : reject) b += "[ \"$1\" = '" + r + "' ] && exit 1\n";
    b += "out=''\nwhile [ $# -gt 0 ]; do case \"$1\" in -o) out=\"$2\"; shift;; esac; shift; done\n";
    if (exe_body.empty()) {
        b += "exit 1\n";
    } else {
        b += "printf '%s\\n' '#!/bin/sh' '" + exe_body + "' > \"$out\"\nchmod +x \"$out\"\n";
    }
    return fake(bin, "gcc", b);
}

prism::FunctionInfo masked_fn(const fs::path& file) {
    auto f = scalar_fn(file);
    f.name = "masked";
    f.signature = "int masked(int x)";
    f.body = "return x & 7;";
    return f;
}
}  // namespace

TEST_CASE("afl harness: sanitizer fallback is ASan+UBSan, then UBSan, then ASan, then bare") {
    NoTools nt;
    TmpDir t;
    auto h = t.put("h.c", "int main(void) { return 0; }\n");
    auto log = t.dir / "cc.log";
    // Both sanitizers together are preferred (TSan is never tried: it cannot
    // combine with ASan).
    fake_cc(nt.bin, log, {}, "exit 0");
    auto [ok, err] = sd::compile_afl_harness(h, t.dir / "a.exe");
    CHECK(ok);
    CHECK(lines_of(log) == std::vector<std::string>{"-fsanitize=address,undefined"});
    // No combined runtime: UBSan alone.
    fs::remove(log);
    fake_cc(nt.bin, log, {"-fsanitize=address,undefined"}, "exit 0");
    std::tie(ok, err) = sd::compile_afl_harness(h, t.dir / "b.exe");
    CHECK(ok);
    CHECK(lines_of(log) == std::vector<std::string>{"-fsanitize=address,undefined", "-fsanitize=undefined"});
    // No UBSan either: ASan alone.
    fs::remove(log);
    fake_cc(nt.bin, log, {"-fsanitize=address,undefined", "-fsanitize=undefined"}, "exit 0");
    std::tie(ok, err) = sd::compile_afl_harness(h, t.dir / "c.exe");
    CHECK(ok);
    CHECK(lines_of(log) == std::vector<std::string>{"-fsanitize=address,undefined", "-fsanitize=undefined",
                                                    "-fsanitize=address"});
    // No sanitizer runtime at all: the bare build, still a build.
    fs::remove(log);
    fake_cc(nt.bin, log, {"-fsanitize=address,undefined", "-fsanitize=undefined", "-fsanitize=address"},
            "exit 0");
    std::tie(ok, err) = sd::compile_afl_harness(h, t.dir / "d.exe");
    CHECK(ok);
    CHECK(lines_of(log) == std::vector<std::string>{"-fsanitize=address,undefined", "-fsanitize=undefined",
                                                    "-fsanitize=address", "-O0"});
    // Nothing builds: not ok, and the compiler's words are the reason.
    fs::remove(log);
    fake(nt.bin, "gcc", "echo \"$1\" >> '" + log.string() + "'\necho 'cc: fatal error: broken' >&2\nexit 1");
    std::tie(ok, err) = sd::compile_afl_harness(h, t.dir / "e.exe");
    CHECK_FALSE(ok);
    CHECK(has(err, "broken"));
    CHECK(lines_of(log).size() == 4);
    for (const auto& l : lines_of(log)) CHECK_FALSE(has(l, "thread"));
}

TEST_CASE("fuzz_function: the binary oracle runs after the concrete one and before CLEAN") {
    NoTools nt;
    prism::sandbox::Policy allow(true);
    EnvVar no_afl("PRISM_AFL", std::nullopt);
    EnvVar no_lf("PRISM_LIBFUZZER", std::nullopt);
    TmpDir t;
    auto log = t.dir / "cc.log";
    auto src = t.put("masked.c", "int masked(int x) { return x & 7; }\n");
    auto fn = masked_fn(src);

    // The concrete oracle finds nothing; the compiled harness crashes: the
    // crash is the binary oracle's.
    fake_cc(nt.bin, log, {}, "kill -SEGV $$");
    auto recs = prism::run_fuse({fn}, {}, t.dir, 0.3, 8, false, nt.cfg);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == laws::CRASH);
    CHECK(recs[0].cls == "FUZZ-CRASH");
    CHECK(xget(recs[0], "oracle") == "binary");
    CHECK(xget(recs[0], "binary_iters") == "1");
    CHECK(xget(recs[0], "sandbox") == prism::sandbox::kind());
    CHECK_FALSE(slurp(log).empty());

    // The harness does not build: the row is still the concrete oracle's
    // CLEAN (not a proof), and it says the binary half could not run.
    fs::remove(log);
    fake_cc(nt.bin, log, {}, "");
    recs = prism::run_fuse({fn}, {}, t.dir, 0.3, 8, false, nt.cfg);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == laws::CLEAN);
    CHECK_FALSE(laws::is_proof(recs[0].status));
    CHECK(has(recs[0].message, "not a proof"));
    CHECK(xget(recs[0], "binary") == "compile-failed");
    CHECK(xget(recs[0], "oracle") == "concrete");
    // every sanitizer set, then bare (once per fuzz round)
    auto tries = lines_of(log);
    REQUIRE(tries.size() >= 4);
    CHECK(std::vector<std::string>(tries.begin(), tries.begin() + 4) ==
          std::vector<std::string>{"-fsanitize=address,undefined", "-fsanitize=undefined", "-fsanitize=address",
                                   "-O0"});

    // A concrete-oracle crash comes first: the harness is never compiled.
    fs::remove(log);
    fake_cc(nt.bin, log, {}, "kill -SEGV $$");
    auto ub = scalar_fn(src);  // x + 1 overflows at INT_MAX
    recs = prism::run_fuse({ub}, {}, t.dir, 0.3, 8, false, nt.cfg);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == laws::CRASH);
    CHECK(xget(recs[0], "oracle") == "concrete");
    CHECK_FALSE(fs::exists(log));

    // A harness that builds and runs quietly: CLEAN from both oracles.
    fake_cc(nt.bin, log, {}, "cat > /dev/null; exit 0");
    recs = prism::run_fuse({fn}, {}, t.dir, 0.3, 8, false, nt.cfg);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == laws::CLEAN);
    CHECK_FALSE(recs[0].extra.contains("binary"));
    CHECK(xget(recs[0], "sandbox") == prism::sandbox::kind());
}

// ---------------------------------------------------------------- rlef_repair

namespace {
// A scripted llama-server: GET /health answers 200; each POST
// /v1/chat/completions takes the next reply ("" closes the connection
// without an answer, as a server that went away does).
struct FakeLlama {
    int lfd = -1;
    int port = 0;
    std::deque<std::string> replies;
    std::atomic<bool> stop{false};
    std::atomic<int> chats{0};
    std::thread th;

    explicit FakeLlama(std::deque<std::string> r) : replies(std::move(r)) {
        lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(lfd >= 0);
        int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        REQUIRE(::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        REQUIRE(::listen(lfd, 8) == 0);
        socklen_t len = sizeof a;
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] { serve(); });
    }
    ~FakeLlama() {
        stop = true;
        th.join();
        ::close(lfd);
    }
    FakeLlama(const FakeLlama&) = delete;
    FakeLlama& operator=(const FakeLlama&) = delete;
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }

    static std::string chat_json(const std::string& content) {
        return nlohmann::json{{"choices", {{{"message", {{"role", "assistant"}, {"content", content}}}}}}}.dump();
    }

    void serve() {
        while (!stop) {
            pollfd p{lfd, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            int c = ::accept(lfd, nullptr, nullptr);
            if (c < 0) continue;
            std::string req;
            char buf[4096];
            std::size_t need = std::string::npos;
            for (;;) {
                auto he = req.find("\r\n\r\n");
                if (he != std::string::npos && need == std::string::npos) {
                    need = he + 4;
                    auto low = lower(req.substr(0, he));
                    auto cl = low.find("content-length:");
                    if (cl != std::string::npos) need += std::stoul(low.substr(cl + 15));
                }
                if (need != std::string::npos && req.size() >= need) break;
                pollfd q{c, POLLIN, 0};
                if (::poll(&q, 1, 2000) <= 0) break;
                auto n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                req.append(buf, static_cast<std::size_t>(n));
            }
            std::string body;
            if (req.starts_with("GET /health") || req.starts_with("GET /v1/models")) {
                body = "{}";
            } else if (req.starts_with("POST /v1/chat/completions")) {
                ++chats;
                if (!replies.empty()) {
                    body = replies.front();
                    replies.pop_front();
                }
            }
            if (!body.empty()) {
                auto resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                (void)::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
            }
            ::close(c);
        }
    }
};
}  // namespace

TEST_CASE("rlef_repair: a lost connection is NOTRUN only before any candidate scored") {
    if (!prism::Config{}.which({"gcc", "clang"})) {
        MESSAGE("no C compiler: RLEF candidates cannot be built here");
        return;
    }
    TmpDir t;
    auto src = t.put("div.c", "int div_param(int x, int y) { return x / y; }\n");
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = std::string(laws::FAILED);
    fail.file = src.string();
    fail.function = "div_param";
    fail.line = 1;
    fail.cls = "INT-DIV-ZERO";
    fail.message = "div by zero";
    auto cfg = prism::default_config();
    cfg.root = t.dir;
    cfg.allow_exec = true;
    cfg.repair_rounds = 3;
    cfg.gguf.clear();
    cfg.ollama_host.clear();
    // A candidate that builds and runs but exits 3: scored (1), not accepted,
    // so the loop asks again. main(argc, argv) has a pointer parameter, so no
    // BMC verdict is attached to it.
    const std::string cand = "int main(int argc, char **argv) { (void)argv; return argc + 2; }\n";

    SUBCASE("the server goes away after a scored candidate: the best candidate stands") {
        FakeLlama srv({FakeLlama::chat_json(cand), ""});
        cfg.llama_server = srv.url();
        auto out = prism::rlef_repair(fail, cfg);
        REQUIRE(out.size() == 1);
        CHECK(srv.chats == 2);
        CHECK(out[0].stage == "repair");
        CHECK(out[0].status != laws::NOTRUN);
        CHECK(out[0].status == laws::HYPOTHESIS);
        CHECK(has(out[0].message, "RLEF best score 1 over 2 rounds (not a proof)"));
        CHECK(has(xget(out[0], "history"), "HTTP error"));
        CHECK(has(xget(out[0], "best"), "argc + 2"));
        CHECK_FALSE(laws::is_proof(out[0].status));
    }
    SUBCASE("the server goes away before any candidate: NOTRUN, never a verdict") {
        FakeLlama srv({""});
        cfg.llama_server = srv.url();
        auto out = prism::rlef_repair(fail, cfg);
        REQUIRE(out.size() == 1);
        CHECK(srv.chats == 1);
        CHECK(out[0].status == laws::NOTRUN);
        CHECK(has(out[0].message, "HTTP error"));
        CHECK(xget(out[0], "backend") == "llama-server");
    }
    SUBCASE("a silent reply scores nothing: a later lost connection is still NOTRUN") {
        // history holds a row for the silent round, so a history-size test
        // would wrongly keep going; best_score is what counts.
        FakeLlama srv({FakeLlama::chat_json("  \n"), ""});
        cfg.llama_server = srv.url();
        auto out = prism::rlef_repair(fail, cfg);
        REQUIRE(out.size() == 1);
        CHECK(srv.chats == 2);
        CHECK(out[0].status == laws::NOTRUN);
        CHECK(has(xget(out[0], "history"), "silent"));
    }
}
#endif
