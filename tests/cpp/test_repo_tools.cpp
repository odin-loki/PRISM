// Repository tooling that used to be scripts: the PLAN.md "done" smoke,
// lean_audit, and fuzz_corpus (astlint_discard → tests/cpp/test_gen_tables.cpp).

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/stages.hpp"

#define LEAN_AUDIT_NO_MAIN
#include "../../src/tools/lean_audit.cpp"
#define PRISM_FUZZ_CORPUS_NO_MAIN
#include "../../src/tools/fuzz_corpus.cpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

fs::path tools_tmp(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("prism-repo-tools-" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

std::vector<std::string> statuses(const prism::RunReport& r, const std::string& stage) {
    std::vector<std::string> out;
    for (const auto& s : r.stages)
        if (s.name == stage)
            for (const auto& f : s.findings) out.push_back(f.status);
    return out;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

prism::RunReport plan_done_run(const std::string& plant, const fs::path& out) {
    prism::Config cfg = prism::default_config();
    cfg.root = repo_root() / "testdata" / plant;
    cfg.out = out;
    cfg.llm = false;
    cfg.jobs = 4;
    cfg.skip = {"llm", "optional", "repair", "execute"};
    return prism::run_pipeline(cfg);
}

}  // namespace

// docs/PLAN.md "done": prove a SCALAR, crash a plant, name a missing tool.
// Without --allow-exec (Law 9) the crash comes from the in-process
// interpreter paths (concolic / fuzz), not from running compiled code.
TEST_CASE("plan done: abs_ok PROVED-UNBOUNDED; oob_write CRASH; missing ESBMC is never a proof") {
    auto tmp = tools_tmp("plan-done");
    REQUIRE(fs::is_regular_file(repo_root() / "testdata" / "abs_ok.c"));
    auto abs = plan_done_run("abs_ok.c", tmp / "abs_ok");
#ifdef PRISM_HAS_Z3
    CHECK(has(statuses(abs, "bmc"), std::string(prism::laws::PROVED_UNBOUNDED)));
#else
    // Without Z3 the bmc stage is NOTRUN (Law 1), never a proof.
    CHECK_FALSE(has(statuses(abs, "bmc"), std::string(prism::laws::PROVED_UNBOUNDED)));
#endif
    // ESBMC is optional: when it is missing it is NOTRUN, never a clean or proved row (Law 1).
    for (const auto& s : statuses(abs, "esbmc")) {
        CHECK(s != prism::laws::CLEAN);
        CHECK(s != prism::laws::PROVED);
        CHECK(s != prism::laws::PROVED_UNBOUNDED);
        CHECK(s != prism::laws::BOUNDED);
    }
    auto oob = plan_done_run("oob_write.c", tmp / "oob_write");
    std::vector<std::string> dyn;
    for (const char* st : {"fuzz", "fuse", "concolic"})
        for (auto& s : statuses(oob, st)) dyn.push_back(s);
    CHECK(has(dyn, std::string(prism::laws::CRASH)));
}

// ------------------------------------------------------------------ lean_audit
TEST_CASE("lean_audit: comments are stripped the Lean way; hits are whole words or line-start keywords") {
    using lean_audit::find_hits;
    using lean_audit::strip_comments;
    const std::vector<std::string> words{"sorry", "native_decide"}, top{"axiom", "unsafe"};
    auto hits = [&](const std::string& src) {
        std::vector<std::string> out;
        for (auto& h : find_hits(strip_comments(src), words, top)) out.push_back(h.word);
        return out;
    };
    using V = std::vector<std::string>;
    CHECK(hits("theorem t : True := by sorry\n") == V{"sorry"});
    CHECK(hits("-- sorry\ntheorem t : True := trivial\n").empty());
    CHECK(hits("/- sorry -/ theorem t : True := trivial\n").empty());
    CHECK(hits("/-- doc: sorry -/\n/-! module doc native_decide -/\n").empty());
    // Block comments nest: the whole thing is a comment.
    CHECK(hits("/- outer /- inner -/ sorry -/\n").empty());
    // ... and code after a nested comment is scanned.
    CHECK(hits("/- a /- b -/ c -/ example := by native_decide\n") == V{"native_decide"});
    // A comment marker inside a string does not hide code; string text is scanned.
    CHECK(hits("def s := \"--\" ++ toString sorry\n") == V{"sorry"});
    CHECK(hits("def s := \"a sorry b\"\n") == V{"sorry"});
    // Whole words only.
    CHECK(hits("theorem mysorry_ok : True := trivial\ndef sorry_count := 0\n").empty());
    CHECK(hits("theorem t := sorry'\n") == V{"sorry"});
    // Keywords count at the start of a line only (after spaces), also after a
    // comment on the lines before.
    CHECK(hits("axiom bad : False\n") == V{"axiom"});
    CHECK(hits("  unsafe def f := 0\n") == V{"unsafe"});
    CHECK(hits("theorem axiom_free : True := trivial\n").empty());
    CHECK(hits("/- a\nb -/\naxiom x : False\n") == V{"axiom"});
    CHECK(hits("def f := 1 -- axiom\n").empty());
    // A block comment keeps its newlines, so what follows it can start a line.
    CHECK(hits("def f := 1 /- c\n-/ axiom x : False\n") == V{"axiom"});
    CHECK(hits("def f := 1 /- c\n-/\naxiom x : False\n") == V{"axiom"});
}

TEST_CASE("lean_audit: scan walks directories, skips .lake, reports path: word; axioms audit") {
    auto tmp = tools_tmp("lean-audit");
    fs::create_directories(tmp / "P" / "Sub");
    fs::create_directories(tmp / ".lake" / "packages");
    std::ofstream(tmp / "P" / "Good.lean") << "-- sorry is fine in a comment\ntheorem a : True := trivial\n";
    std::ofstream(tmp / "P" / "Sub" / "Bad.lean") << "theorem b : True := by\n  sorry\n";
    std::ofstream(tmp / ".lake" / "packages" / "Dep.lean") << "theorem c : True := sorry\n";
    std::ofstream(tmp / "notes.txt") << "sorry\n";
    const std::string dir = tmp.string();
    {
        std::vector<std::string> args{"lean_audit", "scan", "--words", "sorry,native_decide", "--toplevel",
                                      "axiom,unsafe", dir};
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        std::ostringstream out, err;
        CHECK(lean_audit::run(static_cast<int>(argv.size()), argv.data(), out, err) == 1);
        CHECK(out.str() == (tmp / "P" / "Sub" / "Bad.lean").generic_string() + ": sorry\n");
    }
    {
        std::vector<std::string> args{"lean_audit", "scan", "--words", "sorry", (tmp / "P" / "Good.lean").string()};
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        std::ostringstream out, err;
        CHECK(lean_audit::run(static_cast<int>(argv.size()), argv.data(), out, err) == 0);
        CHECK(out.str() == "ok\n");
    }
    {  // a missing path is an error, never "ok"
        std::vector<std::string> args{"lean_audit", "scan", "--words", "sorry", (tmp / "missing").string()};
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        std::ostringstream out, err;
        CHECK(lean_audit::run(static_cast<int>(argv.size()), argv.data(), out, err) == 2);
        CHECK(out.str().empty());
    }
    const std::set<std::string> std_axioms{"propext", "Classical.choice", "Quot.sound"};
    CHECK(lean_audit::axiom_offenders("'Prism.a' depends on axioms: [propext, Quot.sound]\n"
                                      "'Prism.b' does not depend on any axioms\n",
                                      std_axioms)
              .empty());
    CHECK(lean_audit::axiom_offenders("'Prism.c' depends on axioms: [propext, sorryAx, Lean.ofReduceBool]\r\n",
                                      std_axioms) == std::vector<std::string>{"Prism.c: ['Lean.ofReduceBool', 'sorryAx']"});
    std::ofstream(tmp / "axioms.txt") << "'x' depends on axioms: [Classical.choice]\n";
    std::vector<std::string> args{"lean_audit", "axioms", (tmp / "axioms.txt").string()};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    std::ostringstream out, err;
    CHECK(lean_audit::run(static_cast<int>(argv.size()), argv.data(), out, err) == 0);
    CHECK(out.str() == "ok: only propext / Classical.choice / Quot.sound\n");
}

// ------------------------------------------------------------------ prism_fuzz_corpus
TEST_CASE("fuzz corpus: seeds are small sources in path order; selector bytes; a missing prism is said") {
    namespace fz = prism_fuzz_corpus;
    CHECK(fz::seed_name("") == "cbf29ce484222325");  // FNV-1a offset basis
    CHECK(fz::seed_name("a") != fz::seed_name("b"));
    CHECK(fz::seed_name("abc").size() == 16);

    auto repo = tools_tmp("fuzz-repo");
    fs::create_directories(repo / "testdata" / "sub");
    fs::create_directories(repo / "tests" / "conformance" / "prism");
    std::ofstream(repo / "testdata" / "b.c") << "int b(void) { return 1; }\n";
    std::ofstream(repo / "testdata" / "sub" / "a.cpp") << "int a() { return 0; }\n";
    std::ofstream(repo / "testdata" / "notes.txt") << "not a seed\n";
    std::ofstream(repo / "testdata" / "big.c") << std::string(70 * 1024, ' ');
    std::ofstream(repo / "tests" / "conformance" / "prism" / "c.c") << "int c(void) { return 2; }\n";
    auto s = fz::seeds({{repo / "testdata", {".c", ".cpp", ".h"}}, {repo / "tests" / "conformance" / "prism", {".c"}}},
                       10);
    REQUIRE(s.size() == 3);
    CHECK(s[0].filename() == "b.c");
    CHECK(s[1].filename() == "a.cpp");
    CHECK(s[2].filename() == "c.c");
    CHECK(fz::seeds({{repo / "testdata", {".c", ".cpp"}}}, 1).size() == 1);

    fz::Options o;
    o.repo = repo;
    o.out = repo / "out";
    o.prism = repo / "no-such-prism";
    std::ostringstream log;
    REQUIRE(fz::make_corpus(o, repo, log) == 0);
    CAPTURE(log.str());
    CHECK(log.str().find("report seeds from a PRISM run: ERROR") != std::string::npos);
    std::size_t c = 0, lf = 0, rep = 0;
    bool c_prefixed = false;
    for (const auto& e : fs::directory_iterator(o.out / "c")) (void)e, ++c;
    for (const auto& e : fs::directory_iterator(o.out / "report")) (void)e, ++rep;
    for (const auto& e : fs::directory_iterator(o.out / "libfuzzer")) {
        ++lf;
        std::ifstream in(e.path(), std::ios::binary);
        std::string d((std::istreambuf_iterator<char>(in)), {});
        if (!d.empty() && d[0] == '\0' && d.find("int b(void)") == 1) c_prefixed = true;
    }
    CHECK(c == 3);
    CHECK(rep == fz::edge_json().size());
    CHECK(c_prefixed);
    CHECK(lf >= c + rep);
    CHECK_FALSE(fs::exists(o.out / "toml"));  // no manifest in this fake repo

    std::ostringstream log2;
    fz::Options empty;
    empty.repo = repo / "nowhere";
    empty.out = repo / "out2";
    CHECK(fz::make_corpus(empty, repo, log2) == 1);  // no seeds: an error, not an empty corpus
}
