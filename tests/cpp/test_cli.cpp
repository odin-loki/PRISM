// `prism PATH [options]` (src/prism/cli.cpp, src/prism/main.cpp): strict
// parsing, --flag=value, defaults, the stage list, and the binary's exit
// codes (Laws 7 and 8: a typo never runs a different scan).

#include <doctest/doctest.h>

#include "prism/cli.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/threads.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <random>
#include <span>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

prism::CliResult<prism::CliOptions> parse(std::initializer_list<const char*> args) {
    std::vector<const char*> v(args);
    return prism::parse_cli(std::span<const char* const>(v.data(), v.size()));
}

std::string error_of(std::initializer_list<const char*> args) {
    auto r = parse(args);
    CHECK_FALSE(r);
    return r ? std::string() : r.error();
}

std::vector<std::string> stage_order() {
    std::vector<std::string> v;
    for (auto* p = prism::STAGE_ORDER; *p; ++p) v.emplace_back(*p);
    return v;
}

}  // namespace

TEST_CASE("cli: defaults match default_config and the documented values") {
    auto r = parse({});
    REQUIRE(r);
    auto d = prism::default_config();
    CHECK(r->path == "testdata");
    CHECK(r->fail_on == "never");
    CHECK(r->cfg.out == fs::path("prism-out"));
    CHECK(r->cfg.unwind == 8);
    CHECK(r->cfg.fuzz_budget == 8.0);
    CHECK(r->cfg.fuzz_iters == 2048);
    CHECK(r->cfg.repair_rounds == 3);
    CHECK(r->cfg.timeout == 30.0);
    CHECK(r->cfg.jobs == d.jobs);
    CHECK(r->cfg.llm);
    CHECK_FALSE(r->cfg.stages);
    CHECK(r->cfg.skip.empty());
    CHECK_FALSE(r->cfg.allow_exec);
    CHECK_FALSE(r->cfg.resume);
    CHECK_FALSE(r->help);
    CHECK_FALSE(r->version);
    CHECK_FALSE(r->list_stages);
    // The help text states the same defaults.
    auto u = prism::cli_usage();
    for (auto* want : {"--unwind 8", "--fuzz-budget 8", "--fuzz-iters 2048", "--repair-rounds 3",
                       "--timeout 30", "--out prism-out"})
        CHECK_MESSAGE(u.find(want) != std::string::npos, want);
}

TEST_CASE("cli: every flag in both spellings, -jN, and the path") {
    auto a = parse({"src", "--out", "o", "--no-llm", "--stage", "bmc,pir", "--skip", "fuzz",
                    "--unwind", "3", "--jobs", "4", "--fuzz-budget", "1.5", "--fuzz-iters", "16",
                    "--repair-rounds", "1", "--resume", "--tool", "esbmc=/opt/esbmc", "--allow-exec",
                    "--certified", "--solver-cache", "sc", "--timeout", "5", "--pbsd", "pb",
                    "--fail-on", "defect", "--strict-aliasing", "--pir-drafts", "--fp-checks",
                    "--requirements", "req.md", "--contracts-approved", "ap.json"});
    REQUIRE(a);
    auto b = parse({"--out=o", "--no-llm", "--stage=bmc,pir", "--skip=fuzz", "--unwind=3", "-j4",
                    "--fuzz-budget=1.5", "--fuzz-iters=16", "--repair-rounds=1", "--resume",
                    "--tool=esbmc=/opt/esbmc", "--allow-exec", "--certified", "--solver-cache=sc",
                    "--timeout=5", "--pbsd=pb", "--fail-on=defect", "--strict-aliasing",
                    "--pir-drafts", "--fp-checks", "--requirements=req.md",
                    "--contracts-approved=ap.json", "src"});
    REQUIRE(b);
    for (auto* r : {&*a, &*b}) {
        CHECK(r->path == "src");
        CHECK(r->cfg.out == fs::path("o"));
        CHECK_FALSE(r->cfg.llm);
        REQUIRE(r->cfg.stages);
        CHECK(*r->cfg.stages == std::vector<std::string>{"bmc", "pir"});
        CHECK(r->cfg.skip == std::vector<std::string>{"fuzz"});
        CHECK(r->cfg.unwind == 3);
        CHECK(r->cfg.jobs == 4);
        CHECK(r->cfg.fuzz_budget == 1.5);
        CHECK(r->cfg.fuzz_iters == 16);
        CHECK(r->cfg.repair_rounds == 1);
        CHECK(r->cfg.resume);
        CHECK(r->cfg.tools.at("esbmc") == fs::path("/opt/esbmc"));
        CHECK(r->cfg.allow_exec);
        CHECK(r->cfg.certified);
        CHECK(r->cfg.solver_cache == fs::absolute("sc"));
        CHECK(r->cfg.timeout == 5.0);
        CHECK(r->cfg.pbsd_root == fs::absolute("pb"));
        CHECK(r->fail_on == "defect");
        CHECK(r->cfg.strict_aliasing);
        CHECK(r->cfg.pir_drafts);
        CHECK(r->cfg.fp_checks);
        CHECK(r->cfg.requirements == std::vector<fs::path>{fs::absolute("req.md")});
        CHECK(r->cfg.contracts_approved == fs::absolute("ap.json"));
    }
    auto j = parse({"-j", "3"});
    REQUIRE(j);
    CHECK(j->cfg.jobs == 3);
    // --jobs 0 is half the cores, resolved so report.json records the count.
    auto z = parse({"--jobs", "0"});
    REQUIRE(z);
    CHECK(z->cfg.jobs == prism::clamp_jobs(0));
    CHECK(z->cfg.jobs >= 1);
    // --requirements repeats; -- ends the options.
    auto rq = parse({"--requirements", "a", "--requirements=b", "--", "-weird-dir"});
    REQUIRE(rq);
    CHECK(rq->cfg.requirements.size() == 2);
    CHECK(rq->path == "-weird-dir");
    auto hidden = parse({"--pir-vcs", "a.c", "--solve-smt2=q.smt2", "--z3-only"});
    REQUIRE(hidden);
    CHECK(hidden->pir_vcs_src == "a.c");
    CHECK(hidden->solve_smt2 == "q.smt2");
    CHECK(hidden->z3_only);
}

TEST_CASE("cli: --help, --version and --list-stages") {
    for (auto* h : {"-h", "--help"}) {
        auto r = parse({h});
        REQUIRE(r);
        CHECK(r->help);
    }
    // Like argparse, help and version act where they appear.
    auto hv = parse({"--no-llm", "--help", "--bogus"});
    REQUIRE(hv);
    CHECK(hv->help);
    for (auto* v : {"-V", "--version"}) {
        auto r = parse({v});
        REQUIRE(r);
        CHECK(r->version);
    }
    auto ls = parse({"--list-stages"});
    REQUIRE(ls);
    CHECK(ls->list_stages);
    CHECK(std::string(prism::PRISM_VERSION) == "0.1.0");

    auto u = prism::cli_usage();
    // What `prism --help` promises: any codebase, the exec gate, fail-on,
    // SARIF, the polyglot stage, resume, and every flag.
    for (auto* want : {"any language", "polyglot", "NOTRUN, never as clean", "--allow-exec",
                       "--fail-on", "warning/note/style", "report.sarif", "--pbsd", "--resume",
                       "stages.jsonl", "--list-stages", "--gui", "--no-llm", "--jobs", "--tool NAME=PATH",
                       "--stage a,b", "--skip a,b", "--out DIR", "--unwind", "--fuzz-budget",
                       "--fuzz-iters", "--repair-rounds", "--version", "--requirements",
                       "--contracts-approved", "--strict-aliasing", "--pir-drafts", "--fp-checks",
                       "--certified", "--solver-cache", "--timeout", "prism prove", "prism regress",
                       "prism ask", "prism draft", "prism triage", "--out=DIR"})
        CHECK_MESSAGE(u.find(want) != std::string::npos, want);
    for (auto* never : {"Qwen", "Hybrid code-testing", "directory of C/C++", "Python engine"})
        CHECK_MESSAGE(u.find(never) == std::string::npos, never);
}

TEST_CASE("cli: an unknown option, a stray argument or a switch with a value is an error") {
    CHECK(error_of({"--bogus-flag"}) == "unrecognized arguments: --bogus-flag");
    CHECK(error_of({"--no-llms"}) == "unrecognized arguments: --no-llms");
    CHECK(error_of({"-x"}) == "unrecognized arguments: -x");
    // An unknown option's value is not taken as the scan PATH.
    CHECK(error_of({"--foo", "bar"}) == "unrecognized arguments: --foo");
    CHECK(error_of({"a", "b"}) == "unrecognized arguments: b");
    CHECK(error_of({"--no-llm=1"}) == "argument --no-llm: ignored explicit argument '1'");
    CHECK(error_of({"--allow-exec=yes"}) == "argument --allow-exec: ignored explicit argument 'yes'");
}

TEST_CASE("cli: a missing or malformed value is an error, never an empty or default value") {
    CHECK(error_of({"--out"}) == "argument --out: expected one argument");
    CHECK(error_of({"--out", "--no-llm"}) == "argument --out: expected one argument");
    CHECK(error_of({"--out="}) == "argument --out: expected a non-empty value");
    CHECK(error_of({"--stage"}) == "argument --stage: expected one argument");
    CHECK(error_of({"-j"}) == "argument -j: expected one argument");
    CHECK(error_of({"--unwind", "abc"}) == "argument --unwind: invalid int value: 'abc'");
    CHECK(error_of({"--unwind=3x"}) == "argument --unwind: invalid int value: '3x'");
    CHECK(error_of({"--unwind", "0"}) == "argument --unwind: must be at least 1, got 0");
    CHECK(error_of({"--jobs", "-1"}) == "argument --jobs: must be at least 0, got -1");
    CHECK(error_of({"-jx"}) == "argument -j: invalid int value: 'x'");
    CHECK(error_of({"--fuzz-iters", "1e3"}) == "argument --fuzz-iters: invalid int value: '1e3'");
    CHECK(error_of({"--repair-rounds", ""}) == "argument --repair-rounds: invalid int value: ''");
    CHECK(error_of({"--timeout", "nan"}) == "argument --timeout: invalid float value: 'nan'");
    CHECK(error_of({"--timeout", "inf"}) == "argument --timeout: invalid float value: 'inf'");
    CHECK(error_of({"--fuzz-budget", "-1"}) == "argument --fuzz-budget: must not be negative, got -1");
    CHECK(error_of({"--fuzz-budget=fast"}) == "argument --fuzz-budget: invalid float value: 'fast'");
    CHECK(error_of({"--unwind", "99999999999"}) == "argument --unwind: invalid int value: '99999999999'");
    auto t = parse({"--timeout", "2.5e1"});
    REQUIRE(t);
    CHECK(t->cfg.timeout == 25.0);
}

TEST_CASE("cli: --fail-on choices and --tool NAME=PATH") {
    CHECK(error_of({"--fail-on=bogus"}) ==
          "argument --fail-on: invalid choice: 'bogus' (choose from never, defect, gap)");
    CHECK(error_of({"--fail-on", "Defect"}).find("invalid choice") != std::string::npos);
    for (auto* f : {"never", "defect", "gap"}) {
        auto r = parse({"--fail-on", f});
        REQUIRE(r);
        CHECK(r->fail_on == f);
    }
    CHECK(error_of({"--tool", "foo"}) == "argument --tool: expects NAME=PATH, got 'foo'");
    CHECK(error_of({"--tool", "=x"}) == "argument --tool: expects NAME=PATH, got '=x'");
    CHECK(error_of({"--tool", "foo="}) == "argument --tool: expects NAME=PATH, got 'foo='");
    auto r = parse({"--tool", " ruff = /x/ruff ", "--tool=cbmc=/a=b"});
    REQUIRE(r);
    CHECK(r->cfg.tools.at("ruff") == fs::path("/x/ruff"));
    CHECK(r->cfg.tools.at("cbmc") == fs::path("/a=b"));  // only the first '=' splits
}

TEST_CASE("cli: --stage/--skip take only pipeline stages; an empty --stage runs every stage") {
    auto e = error_of({"--stage", "bcm"});
    CHECK(e.starts_with("argument --stage: unknown stage 'bcm' (stages: inventory, classify,"));
    CHECK(e.find("unify") != std::string::npos);
    CHECK(error_of({"--skip", "fuse"}).starts_with("argument --skip: unknown stage 'fuse'"));
    CHECK(error_of({"--stage=bmc,,nope"}).starts_with("argument --stage: unknown stage 'nope'"));
    for (auto* empty : {"", " , "}) {
        auto r = parse({"--stage", empty});
        REQUIRE(r);
        CHECK_FALSE(r->cfg.stages);  // every stage, as if --stage were not given
        for (auto& s : stage_order()) CHECK(r->cfg.want(s));
    }
    auto sp = parse({"--stage", " bmc , pir ", "--skip", ""});
    REQUIRE(sp);
    CHECK(*sp->cfg.stages == std::vector<std::string>{"bmc", "pir"});
    CHECK(sp->cfg.skip.empty());
    // Every stage name is accepted.
    std::string all;
    for (auto& s : stage_order()) all += (all.empty() ? "" : ",") + s;
    auto every = parse({"--stage", all.c_str(), "--skip", all.c_str()});
    REQUIRE(every);
    CHECK(every->cfg.stages->size() == stage_order().size());
}

TEST_CASE("cli: prism triage options") {
    auto run = [](std::initializer_list<const char*> args) {
        std::vector<const char*> v(args);
        return prism::parse_triage_cli(std::span<const char* const>(v.data(), v.size()));
    };
    auto d = run({});
    REQUIRE(d);
    CHECK(d->report_dir == "prism-out");
    CHECK(d->threshold < 0);
    CHECK(d->use_embedder);
    auto a = run({"out", "--threshold", "0.5", "--no-embed"});
    REQUIRE(a);
    CHECK(a->report_dir == "out");
    CHECK(a->threshold == 0.5);
    CHECK_FALSE(a->use_embedder);
    auto b = run({"--threshold=0.9", "o2"});
    REQUIRE(b);
    CHECK(b->threshold == 0.9);
    CHECK(b->report_dir == "o2");
    CHECK_FALSE(run({"--threshold", "x"}));
    CHECK(run({"--threshold", "x"}).error() == "argument --threshold: invalid float value: 'x'");
    CHECK_FALSE(run({"--threshold", "2"}));
    CHECK(run({"--bogus"}).error() == "unrecognized arguments: --bogus");
    CHECK(run({"a", "b"}).error() == "unrecognized arguments: b");
}

// ---- the stage list ------------------------------------------------------------

TEST_CASE("pipeline: every STAGE_ORDER name is run, in order (none skipped quietly)") {
    for (auto* s : {"harness", "rapid", "muttest", "diff", "fuzz", "bmc", "execute", "concolic",
                    "sanitize", "taint", "thread", "interval", "wp", "polyglot", "pir", "conc",
                    "review", "unify"}) {
        auto o = stage_order();
        CHECK_MESSAGE(std::find(o.begin(), o.end(), s) != o.end(), s);
    }
    std::random_device rd;
    auto out = fs::temp_directory_path() / ("prism_order_" + std::to_string(rd()));
    auto cfg = prism::default_config();
    cfg.root = out / "src";
    fs::create_directories(cfg.root);
    cfg.out = out / "out";
    cfg.llm = false;
    cfg.skip = stage_order();
    auto rep = prism::run_pipeline(cfg);
    std::vector<std::string> names;
    for (auto& s : rep.stages) {
        names.push_back(s.name);
        CHECK(s.status == "skipped");
        CHECK(s.detail == "excluded by --stage/--skip");
    }
    CHECK(names == stage_order());
    std::error_code ec;
    fs::remove_all(out, ec);
}

// ---- the binary ------------------------------------------------------------------

#ifndef _WIN32
namespace {

fs::path prism_bin() {
    char buf[4096]{};
    auto n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? fs::path(std::string(buf, static_cast<std::size_t>(n))).parent_path() / "prism"
                 : fs::path();
}

struct Run {
    int rc = -1;
    std::string out;
};

Run run_prism(const std::string& args) {
    Run r;
    std::string cmd = "'" + prism_bin().string() + "' " + args + " 2>&1";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return r;
    char buf[4096];
    while (auto n = std::fread(buf, 1, sizeof buf, p)) r.out.append(buf, n);
    int st = ::pclose(p);
    r.rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return r;
}

}  // namespace

TEST_CASE("prism binary: exit 2 with a message on bad arguments, never an abort or a scan") {
    REQUIRE_MESSAGE(fs::exists(prism_bin()), "prism is built next to prism_tests");
    for (auto* bad : {"--bogus-flag", "--unwind x", "--unwind=x", "--jobs x", "--fuzz-iters x",
                      "--repair-rounds x", "--timeout x", "--fuzz-budget x", "--tool foo",
                      "--stage bcm", "--stage", "--fail-on=bogus", "a b", "triage --threshold x"}) {
        auto r = run_prism(bad);
        CHECK_MESSAGE(r.rc == 2, bad << " -> " << r.rc << " " << r.out);
        CHECK_MESSAGE(r.out.find("error") != std::string::npos, bad);
        CHECK_MESSAGE(r.out.find("confidence") == std::string::npos, bad);  // no scan ran
    }
    auto missing = run_prism("/nonexistent/prism_typo_dir --no-llm --fail-on defect --stage inventory");
    CHECK(missing.rc == 2);
    CHECK(missing.out.find("PATH does not exist") != std::string::npos);

    auto ls = run_prism("--list-stages");
    CHECK(ls.rc == 0);
    std::string want;
    for (auto& s : stage_order()) want += s + "\n";
    CHECK(ls.out == want);
    auto v = run_prism("--version");
    CHECK(v.rc == 0);
    CHECK(v.out == std::string("prism ") + prism::PRISM_VERSION + " (C++ engine)\n");
    auto h = run_prism("--help");
    CHECK(h.rc == 0);
    CHECK(h.out == prism::cli_usage());
}

TEST_CASE("prism binary: --fail-on=defect is honoured (conflict marker -> exit 1)") {
    REQUIRE(fs::exists(prism_bin()));
    std::random_device rd;
    auto td = fs::temp_directory_path() / ("prism_failon_" + std::to_string(rd()));
    fs::create_directories(td / "src");
    std::ofstream(td / "src" / "a.c") << "<<<<<<< HEAD\nint x;\n>>>>>>> b\n";
    const std::string base = "'" + (td / "src").string() + "' --no-llm --stage polyglot --out '" +
                             (td / "out").string() + "'";
    for (auto* f : {" --fail-on=defect", " --fail-on defect", " --fail-on=gap"}) {
        auto r = run_prism(base + f);
        CHECK_MESSAGE(r.rc == 1, f << ": " << r.out);
    }
    CHECK(run_prism(base).rc == 0);
    CHECK(run_prism(base + " --fail-on=never").rc == 0);
    std::error_code ec;
    fs::remove_all(td, ec);
}
#endif
