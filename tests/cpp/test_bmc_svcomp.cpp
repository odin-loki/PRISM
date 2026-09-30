// Doctests: bmc front end on SV-COMP shaped code (docs/SVCOMP.md): source
// columns after block comments, inlining of the unit's own helpers, main's
// unused argv, printf, gotos into an else branch and self-loop labels, and
// multi-dimensional arrays. Linked into prism_tests next to test_main.cpp
// (which provides main()).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/stages.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

std::vector<prism::FunctionInfo> parse_source(const std::string& name, const std::string& src) {
    auto dir = std::filesystem::temp_directory_path() / "prism_bmc_svcomp";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    {
        std::ofstream out(path, std::ios::binary);
        out << src;
    }
    return prism::extract_functions(path, path.string());
}

[[maybe_unused]] std::map<std::string, prism::Finding> bmc_run(const std::string& name, const std::string& src,
                                                               int unwind = 8) {
    std::map<std::string, prism::Finding> by;
    for (auto& f : prism::run_bmc(parse_source(name, src), unwind))
        if (f.function) by[*f.function] = f;
    return by;
}

[[maybe_unused]] const std::string kProved = std::string(prism::laws::PROVED_UNBOUNDED);
[[maybe_unused]] const std::string kFailed = std::string(prism::laws::FAILED);
[[maybe_unused]] const std::string kHarness = std::string(prism::laws::NEEDS_HARNESS);

}  // namespace

TEST_CASE("cparse: comment_col_shifts records the dropped block-comment delimiters") {
    // `  /* c */ x`: `/*` dropped before stripped col 3, `*/` before col 6.
    auto sh = prism::comment_col_shifts("int a;\n  /* c */ x\n/* two\n lines */ y /**/z\n");
    REQUIRE(sh.size() == 5);
    CHECK(sh[0] == std::array<int, 3>{2, 3, 2});
    CHECK(sh[1] == std::array<int, 3>{2, 6, 2});
    CHECK(sh[2] == std::array<int, 3>{3, 1, 2});
    CHECK(sh[3] == std::array<int, 3>{4, 8, 2});
    // an empty comment drops 4 characters at one column
    CHECK(sh[4] == std::array<int, 3>{4, 11, 4});
    prism::FunctionInfo fn;
    fn.col_shifts = sh;
    CHECK(prism::source_col(fn, 2, 7) == 11);   // x
    CHECK(prism::source_col(fn, 4, 9) == 11);   // y
    CHECK(prism::source_col(fn, 4, 11) == 17);  // z
    CHECK(prism::source_col(fn, 1, 5) == 5);
    // A string holding comment markers is not a comment.
    CHECK(prism::comment_col_shifts("char *s = \"/* x */\";\n").empty());
}

TEST_CASE("inline: a non-static helper is inlined into main only") {
    auto fns = parse_source("inl_nonstatic.c", R"(int add1(int x) { return x + 1; }
int main(void) { int a = 5; int b = add1(a); return b; }
int other(int a) { int b = add1(a); return b; }
)");
    auto out = prism::inline_static(fns);
    std::map<std::string, std::string> body;
    for (auto& f : out) body[f.name] = f.body;
    CHECK(body["main"].find("add1(") == std::string::npos);
    CHECK(body["main"].find("_h0_ret") != std::string::npos);
    // `other` is not the program: another unit's definition may run there.
    CHECK(body["other"].find("add1(") != std::string::npos);
}

TEST_CASE("inline: calls in if, else and loop bodies are inlined, each site with its prefix") {
    auto fns = parse_source("inl_nested.c", R"(static int inc(int x) { return x + 1; }
int f(int a) {
    int r = 0;
    if (a > 0) r = inc(a); else { r = inc(0); }
    while (r < 3) r = inc(r);
    for (int i = 0; i < 2; i++) { r = inc(r); }
    do r = inc(r); while (r < 10);
    return r;
}
)");
    auto out = prism::inline_static(fns);
    const prism::FunctionInfo* f = nullptr;
    for (auto& x : out)
        if (x.name == "f") f = &x;
    REQUIRE(f);
    CHECK(f->body.find("inc(") == std::string::npos);
    for (auto* p : {"_h0_ret", "_h1_ret", "_h2_ret", "_h3_ret", "_h4_ret"}) CHECK(f->body.find(p) != std::string::npos);
    CHECK(f->body_line == 0);  // no longer maps onto the source
}

#ifdef PRISM_HAS_Z3
TEST_CASE("bmc: nondet call columns are source columns after a block comment") {
    auto by = bmc_run("nondet_comment.c", R"(extern int __VERIFIER_nondet_int(void);
int main(void) {
  /* c */ int a = __VERIFIER_nondet_int();
  if (a > 2147483000) { int b = a + 1000; return b; }
  return 0;
}
)");
    REQUIRE(by.count("main"));
    auto& f = by["main"];
    REQUIRE(f.status == kFailed);
    // the call's identifier starts at column 19 of line 3 in the file
    CHECK(f.extra["nondet_loc"] == "3:19");
}

TEST_CASE("bmc: a loop after a block comment keeps its source column in invariant_loops") {
    // testdata/ai_invariants.c ai_sum_to_n with a comment before `while`.
    auto by = bmc_run("loop_comment.c", R"(int sum_to_n(int n) {
    int i;
    int s;
    s = 0;
    i = 0;
    if (n > 1000) return 0;
    /* c */ while (i < n) {
        s = s + 2;
        i = i + 1;
    }
    return s;
}
)");
    REQUIRE(by.count("sum_to_n"));
    auto& r = by["sum_to_n"];
    REQUIRE(r.status == kProved);
    REQUIRE(r.extra.count("invariant_loops"));
    CHECK(r.extra["invariant_loops"] == R"([{"column":13,"kind":"while","line":7}])");
}

TEST_CASE("bmc: a non-static helper of main is inlined; inline, weak and duplicate ones are not") {
    const std::string bad = R"(extern int __VERIFIER_nondet_int(void);
HEAD int add1(int x) { return x + 1; }
int main(void) { int a = __VERIFIER_nondet_int(); int b = add1(a); return b; }
)";
    auto with = [&](const std::string& head) {
        auto src = bad;
        src.replace(src.find("HEAD"), 4, head);
        return src;
    };
    auto plain = bmc_run("nonstatic_bad.c", with(""));
    CHECK(plain["main"].status == kFailed);
    CHECK(plain["main"].cls == "INT-SIGNED-OVF");
    auto ok = bmc_run("nonstatic_ok.c", R"(extern int __VERIFIER_nondet_int(void);
int add1(int x) { if (x > 1000) return 0; return x + 1; }
int main(void) { int a = __VERIFIER_nondet_int(); int b = add1(a); return b; }
)");
    CHECK(ok["main"].status == kProved);
    for (auto* head : {"inline", "__inline__", "__attribute__((weak))"}) {
        auto r = bmc_run("nonstatic_kept.c", with(head));
        INFO(head);
        CHECK(r["main"].status == kHarness);
        CHECK(r["main"].message.find("add1") != std::string::npos);
    }
    // Two definitions in the tree: not certain which one runs.
    auto fns = parse_source("nonstatic_a.c", with(""));
    auto other = parse_source("nonstatic_b.c", "int add1(int x) { return 0; }\n");
    fns.insert(fns.end(), other.begin(), other.end());
    for (auto& f : prism::run_bmc(fns, 8))
        if (f.function && *f.function == "main") CHECK(f.status == kHarness);
}

TEST_CASE("bmc: calls inside if and while bodies of main are inlined and checked") {
    auto in_if = bmc_run("inl_if.c", R"(extern int __VERIFIER_nondet_int(void);
int add1(int x) { return x + 1; }
int main(void) {
    int a = __VERIFIER_nondet_int();
    int g = 0;
    if (a > 0) {
        g = add1(a);
    }
    return g;
}
)");
    CHECK(in_if["main"].status == kFailed);
    CHECK(in_if["main"].cls == "INT-SIGNED-OVF");
    auto in_while = bmc_run("inl_while.c", R"(int inc(int x) { return x + 1; }
int main(void) {
    int i = 0;
    while (i < 3) i = inc(i);
    return 10 / (i - 2);
}
)");
    CHECK(in_while["main"].status == kProved);
    auto in_while_bad = bmc_run("inl_while_bad.c", R"(int inc(int x) { return x + 1; }
int main(void) {
    int i = 0;
    while (i < 3) i = inc(i);
    return 10 / (i - 3);
}
)");
    CHECK(in_while_bad["main"].status == kFailed);
    CHECK(in_while_bad["main"].cls == "INT-DIV-ZERO");
}

TEST_CASE("bmc: a static helper that calls __VERIFIER_assert is inlined and its assert checked") {
    auto bad = bmc_run("vassert_helper_bad.c", R"(extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assert(int cond);
static void check(int v) { __VERIFIER_assert(v >= 0); }
int main(void) { int x = __VERIFIER_nondet_int(); check(x); return 0; }
)");
    CHECK(bad["main"].status == kFailed);
    CHECK(bad["main"].cls == "FUNC-CONTRACT");
    auto ok = bmc_run("vassert_helper_ok.c", R"(extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assert(int cond);
static void check(int v) { __VERIFIER_assert(v >= 0); }
int main(void) { int x = __VERIFIER_nondet_int(); if (x < 0) x = 0; check(x); return 0; }
)");
    CHECK(ok["main"].status == kProved);
}

TEST_CASE("bmc: main(argc, argv) with argv unused is checked with argc >= 0") {
    auto unused = bmc_run("argv_unused.c", R"(extern int __VERIFIER_nondet_int(void);
int main(int argc, char *argv[]) {
    int x = __VERIFIER_nondet_int();
    if (x > 2147483000) return x + 1000;
    return 0;
}
)");
    CHECK(unused["main"].status == kFailed);
    CHECK(unused["main"].cls == "INT-SIGNED-OVF");
    auto clean = bmc_run("argv_clean.c", "int main(int argc, char **argv) { return 0; }\n");
    CHECK(clean["main"].status == kProved);
    // argc - 1 cannot overflow for argc >= 0; argc + 1 can (argc = INT_MAX).
    auto dec = bmc_run("argc_dec.c", "int main(int argc, char *argv[]) { int k = argc - 1; return k; }\n");
    CHECK(dec["main"].status == kProved);
    auto inc = bmc_run("argc_inc.c", "int main(int argc, char *argv[]) { return argc + 1; }\n");
    CHECK(inc["main"].status == kFailed);
    // Any use of argv or envp keeps Law 6.
    auto used = bmc_run("argv_used.c", "int main(int argc, char *argv[]) { return argv[0][0]; }\n");
    CHECK(used["main"].status == kHarness);
    CHECK(used["main"].message.find("pointer parameter") != std::string::npos);
    auto envp = bmc_run("envp_used.c",
                        "int main(int argc, char **argv, char **envp) { return envp != 0; }\n");
    CHECK(envp["main"].status == kHarness);
    auto macro = bmc_run("argv_macro.c", "#define FIRST argv[0]\nint main(int argc, char **argv) { return FIRST != 0; }\n");
    CHECK(macro["main"].status == kHarness);
    // Not main: an ordinary pointer parameter.
    auto notmain = bmc_run("argv_notmain.c", "int f(int argc, char **argv) { return 0; }\n");
    CHECK(notmain["f"].status == kHarness);
    // A tree that calls main may pass a negative argc: no premise.
    auto called = bmc_run("main_called.c", R"(int main(int argc, char *argv[]) { int k = argc - 1; return k; }
int g(void) { return 0; }
int h(void) { return main(0, 0); }
)");
    CHECK(called["main"].status == kFailed);
}

TEST_CASE("bmc: printf, puts and putchar are modelled for literal formats and scalar arguments") {
    auto by = bmc_run("printf.c", R"(extern int printf(const char *fmt, ...);
extern int puts(const char *s);
extern int putchar(int c);
int p_ovf(int x) { printf("%d\n", x + 1); return 0; }
int p_only(int x) { printf("%d\n", x); return 0; }
int p_ok(int x) { if (x > 100) return 0; printf("x=%d %s %-5lx%%\n", x + 1, "ok", (long)x); puts("done"); putchar(x); return 0; }
int p_n(int x) { printf("%d%n\n", x, x); return 0; }
int p_var(int x) { char s[3] = "%d"; printf(s, x); return 0; }
int p_count(int x) { printf("%d %d\n", x); return 0; }
int p_width(int x) { printf("%ld\n", x); return 0; }
int p_star(int x) { printf("%*d\n", x, x); return 0; }
int p_esc(int x) { printf("\045n", x); return 0; }
int p_result(int x) { int r = printf("hi\n"); if (r == 1000) return x / 0; return 0; }
)");
    CHECK(by["p_ovf"].status == kFailed);
    CHECK(by["p_ovf"].cls == "INT-SIGNED-OVF");
    // No property besides the call: a modelled printf is not a libc effect
    // that blocks the proof.
    CHECK(by["p_only"].status == kProved);
    INFO(by["p_ok"].message);
    CHECK(by["p_ok"].status == kProved);
    for (auto* name : {"p_n", "p_var", "p_count", "p_width", "p_star", "p_esc"}) {
        INFO(name << ": " << by[name].message);
        CHECK(by[name].status == kHarness);
        CHECK(by[name].message.find("printf") != std::string::npos);
    }
    // The result is any value: a violation that needs one is neither a
    // refutation nor excluded.
    INFO(by["p_result"].message);
    CHECK(by["p_result"].status == kHarness);
}

TEST_CASE("bmc goto: into the start of an else branch, and self-loop labels end the path") {
    auto td = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "testdata" /
              "goto_else_stuck.c";
    auto fns = prism::extract_functions(td, "goto_else_stuck.c");
    REQUIRE(fns.size() == 4);
    std::map<std::string, prism::Finding> by;
    for (auto& f : prism::run_bmc(fns, 8))
        if (f.function) by[*f.function] = f;
    CHECK(by["goto_else_bad"].status == kFailed);
    CHECK(by["goto_else_bad"].cls == "INT-DIV-ZERO");
    CHECK(by["goto_else_ok"].status == kProved);
    CHECK(by["goto_stuck_ok"].status == kProved);
    CHECK(by["goto_stuck_bad"].status == kFailed);
    CHECK(by["goto_stuck_bad"].cls == "INT-SIGNED-OVF");
    // A label whose statement is more than the goto is an ordinary label.
    auto loop = bmc_run("stuck_not.c", R"(int f(int i) {
    if (i >= 100) { L: i = i + 1; goto L; }
    return 0;
}
)");
    CHECK(loop["f"].status != kProved);
    // A goto into the middle of an else branch stays unencoded.
    auto mid = bmc_run("else_mid.c", R"(int f(int x) {
    if (x) {
        goto l;
    } else {
        x = 2;
    l:
        x = 1;
    }
    return x;
}
)");
    CHECK(mid["f"].status == kHarness);
    CHECK(mid["f"].message.find("unstructured goto unencoded") != std::string::npos);
    // Names the then branch declared are out of scope at the label.
    auto decl = bmc_run("else_decl.c", R"(int f(int x) {
    if (x) {
        int t = 5;
        if (t > x) goto l;
        x = t;
    } else {
    l:
        x = 1;
    }
    return 10 / x;
}
)");
    CHECK(decl["f"].status == kProved);
}
#endif
