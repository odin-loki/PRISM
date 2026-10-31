// run_harness (include/prism/stages.hpp). A harness assumption is not an
// unconditional proof.
//
//   PROVED           -> PROVED-ASSUMING
//   PROVED-UNBOUNDED -> PROVED-ASSUMING
//   BOUNDED          -> BOUNDED
//   FAILED           -> FAILED
//   NOTRUN           -> NOTRUN
//
// the pir check always asks for an unbounded answer, so a closed unwinding is
// recorded as PROVED-UNBOUNDED. The same assignment lowers a literal PROVED.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/stages.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

prism::FunctionInfo fn_in(const char* file, const char* name) {
    auto p = std::filesystem::path(PRISM_SOURCE_DIR) / "testdata" / "planted" / file;
    for (auto& f : prism::extract_functions(p, p.string()))
        if (f.name == name) return f;
    FAIL("missing function " << name << " in " << file);
    return {};
}

// Same shape as tests/cpp/test_wp_harness.cpp: a tiny C file, parsed back.
prism::FunctionInfo fn_from(const std::string& stem, const std::string& src, const char* name) {
    auto dir = std::filesystem::temp_directory_path() / "prism_harness_assumed";
    std::filesystem::create_directories(dir);
    auto p = dir / stem;
    std::ofstream(p, std::ios::binary) << src;
    for (auto& f : prism::extract_functions(p, p.string()))
        if (f.name == name) return f;
    FAIL("missing function " << name << " in " << stem);
    return {};
}

std::string extra_or(const prism::Finding& f, const char* key) {
    auto it = f.extra.find(key);
    return it == f.extra.end() ? std::string() : it->second;
}

prism::Finding harness_of(const prism::FunctionInfo& fn) {
    REQUIRE(fn.kind == "POINTER");
    auto recs = prism::run_harness({fn}, 8);
    REQUIRE(recs.size() == 1);
    return recs[0];
}

void expect_status(const prism::Finding& r, std::string_view original, std::string_view status) {
    INFO(r.status << ": " << r.message);
    CHECK(r.stage == "harness");
    CHECK(extra_or(r, "harness") == "true");
    CHECK(extra_or(r, "original_status") == original);
    CHECK(r.status == status);
    const bool proof = original == prism::laws::PROVED || original == prism::laws::PROVED_UNBOUNDED;
    if (proof) {
        CHECK(r.status == prism::laws::PROVED_ASSUMING);
        CHECK(r.status != prism::laws::PROVED);
        CHECK(r.status != prism::laws::PROVED_UNBOUNDED);
        CHECK(r.message.find("never an unconditional") != std::string::npos);
    } else {
        CHECK(r.status == original);
        CHECK(r.status != prism::laws::PROVED_ASSUMING);
        CHECK(r.message.find("never an unconditional") == std::string::npos);
    }
}

}  // namespace

#ifdef PRISM_HAS_Z3

TEST_CASE("harness assumed: PROVED and PROVED-UNBOUNDED become PROVED-ASSUMING") {
    // deref_ok is the smallest honest pointer the harness tests already use.
    // No loop, so the unwinding closes. Public BMC records that as
    // PROVED-UNBOUNDED; a PROVED answer takes the same lowering.
    auto closed = harness_of(fn_in("ptr_copy.c", "deref_ok"));
    auto closed_orig = extra_or(closed, "original_status");
    INFO("closed original=" << closed_orig);
    CHECK((closed_orig == prism::laws::PROVED || closed_orig == prism::laws::PROVED_UNBOUNDED));
    expect_status(closed, closed_orig, prism::laws::PROVED_ASSUMING);

    // kinduct_closed, plus the smallest honest pointer (p != 0). The step
    // closes, so the BMC answer is PROVED-UNBOUNDED rather than BOUNDED.
    auto unb = harness_of(fn_from("h_unb.c",
                                  "int h_unb(int *p, int n) {\n"
                                  "    // requires: p != 0\n"
                                  "    *p = 1;\n"
                                  "    while (n > 0) {\n"
                                  "        n = n - 1;\n"
                                  "    }\n"
                                  "    return n;\n"
                                  "}\n",
                                  "h_unb"));
    CHECK(extra_or(unb, "k_induction") == "closed");
    expect_status(unb, prism::laws::PROVED_UNBOUNDED, prism::laws::PROVED_ASSUMING);
}

TEST_CASE("harness assumed: BOUNDED stays BOUNDED") {
    // kind_open: the division by zero is past the unwind, so the step stays
    // open and the base case is not promoted and not relabelled PROVED.
    auto r = harness_of(fn_from("h_bounded.c",
                                "int h_bounded(int *p, int n) {\n"
                                "    // requires: p != 0\n"
                                "    int i = 0;\n"
                                "    *p = 1;\n"
                                "    while (i < n && i < 1000) {\n"
                                "        i = i + 1;\n"
                                "    }\n"
                                "    return 100 / (i - 500);\n"
                                "}\n",
                                "h_bounded"));
    expect_status(r, prism::laws::BOUNDED, prism::laws::BOUNDED);
}

TEST_CASE("harness assumed: FAILED stays FAILED") {
    // div_param, on a materialized pointer so the harness stage records it.
    auto r = harness_of(fn_from("h_fail.c",
                                "int h_fail(int *p, int y) {\n"
                                "    // requires: p != 0\n"
                                "    *p = 1;\n"
                                "    return 1 / y;\n"
                                "}\n",
                                "h_fail"));
    expect_status(r, prism::laws::FAILED, prism::laws::FAILED);
}

#else

TEST_CASE("harness assumed: NOTRUN stays NOTRUN") {
    auto recs = prism::run_harness({fn_in("ptr_copy.c", "deref_ok")}, 8);
    REQUIRE(recs.size() == 1);
    CHECK(extra_or(recs[0], "original_status") == prism::laws::NOTRUN);
    CHECK(recs[0].status == prism::laws::NOTRUN);
    CHECK(recs[0].status != prism::laws::PROVED_ASSUMING);
    CHECK(recs[0].status != prism::laws::PROVED);
    CHECK(recs[0].message.find("z3") != std::string::npos);
}

#endif
