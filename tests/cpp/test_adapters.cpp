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
#include <chrono>
#include <cstdlib>
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
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace laws = prism::laws;
namespace ad = prism::adapters_detail;
namespace sd = prism::stages_detail;

namespace {

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }
fs::path testdata() { return repo_root() / "testdata"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
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

    // A program that is not there: exec fails in the child (127), not a verdict.
    prism::detail::RunSpec m;
    m.argv = {(t.dir / "no-such-program").string()};
    m.timeout_s = 5;
    CHECK(prism::detail::run(m).rc == 127);

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

TEST_CASE("config: every pinned external stage has a fetch_deps hint (bitwuzla too)") {
    // [[component]] rows of kind "external": each of their stages maps to the
    // component, so adapter_install names its fetch_deps command.
    std::ifstream in(repo_root() / "third_party" / "MANIFEST.toml");
    std::string line, name, kind;
    int checked = 0;
    while (std::getline(in, line)) {
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
            CHECK_MESSAGE(has(hint, "fetch_deps.py --tool " + name + " "), stage << " -> " << hint);
            CHECK_MESSAGE(prism::pinned_commit(name).has_value(), name);
            ++checked;
        }
    }
    CHECK(checked >= 15);
    CHECK(has(prism::adapter_install("bitwuzla"), "fetch_deps.py --tool bitwuzla"));
    auto esbmc = prism::adapter_install("esbmc");
    CHECK(has(esbmc, "python scripts/fetch_deps.py --tool esbmc"));
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
                CHECK(has(install, "scripts/fetch_deps.py --tool"));
            if (f.stage == "libfuzzer") {
                CHECK(f.message == "clang not on PATH");
            } else {
                CHECK(install == prism::adapter_install(f.stage));
                CHECK(has(f.message, "not found"));
            }
        }
        CHECK(has(xget(stage_row(out, "afl-fuzz"), "install"), "fetch_deps.py --tool aflplusplus"));
        CHECK(has(xget(stage_row(out, "infer"), "install"), "fetch_deps.py --tool infer"));
        CHECK(has(xget(stage_row(out, "frama-c"), "install"), "fetch_deps.py --tool frama-c"));
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

    fake(nt.bin, "clang", "exit 0");
    lf = stage_row(prism::run_optional_tools({}, nt.cfg), "libfuzzer");
    CHECK(lf.status == laws::UNKNOWN);
    never_clean_or_proof(lf);
    CHECK(has(lower(lf.message), "not a code verdict"));
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
    cfg.tools["cppcheck"] = nt.t.put("t/cppcheck-noexec", "#!/bin/sh\n");  // no execute bit
    out = prism::run_cppcheck(paths, cfg);
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == laws::NOTRUN);
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
#endif
