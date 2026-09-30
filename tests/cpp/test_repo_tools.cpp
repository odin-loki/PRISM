// Repository tooling that used to be scripts: the PLAN.md "done" smoke and
// the build-time generator of the Clang-AST discarded-return table.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/stages.hpp"
#include "prism/taxonomy.hpp"

#define PRISM_GEN_DISCARD_NO_MAIN
#include "../../src/tools/gen_astlint_discard.cpp"
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
    CHECK(has(statuses(abs, "bmc"), std::string(prism::laws::PROVED_UNBOUNDED)));
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

// ------------------------------------------------------------------ astlint_discard.inc
namespace {
struct DiscardRow {
    const char* callee;
    std::pair<const char*, bool> v;
};
const DiscardRow kDiscard[] = {
#include "prism/astlint_discard.inc"
};
}  // namespace

TEST_CASE("astlint discard table: generated from the regex rules; every class is in the taxonomy") {
    std::set<std::string> ids;
    for (const auto& c : prism::taxonomy_classes()) ids.insert(c.id);
    std::set<std::string> classes, callees;
    for (const auto& r : kDiscard) {
        CAPTURE(r.callee);
        CHECK(ids.count(r.v.first) == 1);
        classes.insert(r.v.first);
        CHECK(callees.insert(r.callee).second);  // one row per callee
    }
    // The family as it stands (docs/USER_GUIDE.md): hundreds of callees.
    CHECK(callees.size() >= 400);
    CHECK(classes.size() >= 200);
    CHECK(callees.count("pthread_rwlock_*") == 1);
    CHECK(callees.count("getdents64") == 1);
    CHECK(callees.count("renameat") == 1);
    CHECK(callees.count("fchflags") == 1);
    CHECK(std::is_sorted(std::begin(kDiscard), std::end(kDiscard),
                         [](const DiscardRow& a, const DiscardRow& b) { return std::string_view(a.callee) < b.callee; }));
}

TEST_CASE("astlint discard generator: the regex subset it expands, and what it refuses") {
    using prism_gen_discard::expand_regex;
    using V = std::vector<std::string>;
    CHECK(expand_regex("(?P<fn>getdents(?:64)?)") == V{"getdents", "getdents64"});
    CHECK(expand_regex("renameat(?!2)") == V{"renameat"});
    CHECK(expand_regex(R"((?P<fn>pthread_rwlock_\w+))") == V{"pthread_rwlock_*"});
    CHECK(expand_regex(R"((?P<fn>pthread_attr_(?:init|destroy|setstack\w*|setdetachstate)))") ==
          V{"pthread_attr_init", "pthread_attr_destroy", "pthread_attr_setstack*", "pthread_attr_setdetachstate"});
    CHECK(expand_regex("(?P<fn>(?:f|l)?chflags)") == V{"chflags", "fchflags", "lchflags"});
    CHECK(expand_regex("(?P<fn>wait[34])") == V{"wait3", "wait4"});
    CHECK(expand_regex("(?:malloc|calloc)") == V{"malloc", "calloc"});
    CHECK(expand_regex("a[bc]+") == V{"a*"});
    CHECK(expand_regex("(?:a|b)+") == V{"*"});
    CHECK(expand_regex(R"(foo\.bar)") == V{"foo.bar"});
    CHECK(expand_regex(R"(\bfoo)") == V{"foo"});
    // Outside the subset: refused, never guessed.
    for (const char* bad : {"a[a-z]", "a.b", R"(a\db)", "(?:ab)+", "a{2}", "a(?=b)", "x[^y]"}) {
        CAPTURE(bad);
        CHECK_THROWS(expand_regex(bad));
    }
}

TEST_CASE("astlint discard generator: scans lint_discarded* calls; first rule per callee wins; skips are named") {
    const std::string src = R"CPP(
static Regex a_re(R"(^\s*\b(?P<fn>foo(?:64)?)\s*\([^;]*\)\s*;\s*$)");
static const Regex b_re(
    R"(^\s*(?:foo|bar)\s*\([^;]*\)\s*;\s*$)" );
static Regex odd_re(R"(^\s*baz\s*\(\)$)");
static Regex dot_re(R"(^\s*q.x\s*\([^;]*\)\s*;\s*$)");
void rules() {
    lint_discarded(funcs, lines, rel, a_re, "API-FOO", "m", out);
    lint_discarded_zero(funcs, lines, rel, b_re, "API-BAR", "m", out);
    lint_discarded_which(funcs, lines, rel, missing_re, "API-X", "m", out);
    lint_discarded(funcs, lines, rel, odd_re, "API-ODD", "m", out);
    lint_discarded(funcs, lines, rel, dot_re, "API-DOT", "m", out);
}
)CPP";
    std::vector<prism_gen_discard::Row> rows;
    std::vector<prism_gen_discard::Skipped> skipped;
    prism_gen_discard::scan_source(src, rows, skipped);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].name == "foo");
    CHECK(rows[1].name == "foo64");
    CHECK(rows[2].name == "foo");
    CHECK(rows[2].zero);
    CHECK(rows[3].name == "bar");
    REQUIRE(skipped.size() == 3);
    CHECK(skipped[0].why == "no regex");
    CHECK(skipped[1].cls == "API-ODD");
    CHECK(skipped[2].why.find("ANY") != std::string::npos);
    const auto text = prism_gen_discard::render(rows);
    CHECK(text.find(R"(    {"foo", {"API-FOO", false}},)") != std::string::npos);  // the first rule wins
    CHECK(text.find(R"(    {"bar", {"API-BAR", true}},)") != std::string::npos);
    CHECK(text.find("\"bar\"") < text.find("\"foo\""));  // sorted
}

// ------------------------------------------------------------------ Coccinelle rules
TEST_CASE("cocci rules: shipped rules/cocci found from the tree and from an install; tree-local rules too") {
    prism::Config cfg = prism::default_config();
    cfg.root = repo_root() / "testdata";
    std::set<std::string> names;
    std::size_t n = 0;
    for (const auto& r : prism::cocci_rules({}, cfg)) {
        names.insert(r.filename().string());
        ++n;
    }
    CHECK(n == names.size());  // each rule once
    for (const char* need : {"memcpy_self.cocci", "realloc_self.cocci", "shift_bit31.cocci", "getenv_null.cocci",
                             "strcpy_self.cocci", "sprintf_unbounded.cocci", "strcat_self.cocci",
                             "strncpy_self.cocci"}) {
        CAPTURE(need);
        CHECK(names.count(need) == 1);
        CHECK(fs::is_regular_file(repo_root() / "rules" / "cocci" / need));
    }
    const auto rules = repo_root() / "rules" / "cocci";
    std::ifstream g(rules / "getenv_null.cocci"), sp(rules / "sprintf_unbounded.cocci");
    std::string gt((std::istreambuf_iterator<char>(g)), {}), st((std::istreambuf_iterator<char>(sp)), {});
    CHECK(gt.find("getenv") != std::string::npos);
    CHECK(gt.find('@') != std::string::npos);
    CHECK(st.find("sprintf") != std::string::npos);
    CHECK(st.find("snprintf") == std::string::npos);

    // An installed layout (<prefix>/share/prism/cocci above the scanned tree)
    // and a rule inside the scanned root.
    auto tmp = tools_tmp("cocci");
    fs::create_directories(tmp / "share" / "prism" / "cocci");
    std::ofstream(tmp / "share" / "prism" / "cocci" / "installed_rule.cocci") << "@r@\nexpression E;\n@@\n- E\n";
    fs::create_directories(tmp / "src" / "sub");
    std::ofstream(tmp / "src" / "sub" / "local.cocci") << "@r@\nexpression E;\n@@\n- E\n";
    std::ofstream(tmp / "src" / "sub" / "a.c") << "int f(void) { return 0; }\n";
    cfg.root = tmp / "src" / "sub";
    std::set<std::string> got;
    for (const auto& r : prism::cocci_rules({tmp / "src" / "sub" / "a.c"}, cfg)) got.insert(r.filename().string());
    CHECK(got.count("installed_rule.cocci") == 1);
    CHECK(got.count("local.cocci") == 1);
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
