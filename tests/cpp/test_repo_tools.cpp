// Repository tooling that used to be scripts: the PLAN.md "done" smoke and
// the build-time generator of the Clang-AST discarded-return table.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/taxonomy.hpp"

#define PRISM_GEN_DISCARD_NO_MAIN
#include "../../src/tools/gen_astlint_discard.cpp"

#include <algorithm>
#include <filesystem>
#include <set>
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
