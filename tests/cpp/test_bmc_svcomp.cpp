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
#endif
