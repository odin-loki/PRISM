// Division by zero and an out-of-range shift in the concrete interpreter
// (src/prism/stages/interp.cpp). Drives stages_detail::execute, the same
// in-process entry as tests/cpp/test_concrete.cpp. Does not spawn a compiler.

#include <doctest/doctest.h>

#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/stages.hpp"

#include "../../src/prism/stages/interp.hpp"

#include <string>
#include <utility>
#include <vector>

namespace {

namespace sd = prism::stages_detail;
using prism::FunctionInfo;

FunctionInfo synth(const std::string& body, std::vector<std::pair<std::string, std::string>> params = {{"int", "x"}},
                   std::string ret = "int") {
    FunctionInfo f;
    f.name = "f";
    f.kind = "SCALAR";
    f.line = 1;
    f.return_type = std::move(ret);
    f.params = std::move(params);
    f.body = body;
    return f;
}

sd::ExecResult run(const std::string& body, sd::Args args = {},
                   std::vector<std::pair<std::string, std::string>> params = {{"int", "x"}},
                   std::string ret = "int") {
    return sd::execute(synth(body, std::move(params), std::move(ret)), args);
}

// execute stores the class on ub. execute_cex and the fuzzer report a
// non-empty ub as CRASH. That class is not CLEAN and not a proof.
void expect_defect(const sd::ExecResult& rec, const char* cls, const char* body) {
    INFO(body);
    CHECK(rec.ub == cls);
    CHECK(rec.error.empty());
    CHECK_FALSE(rec.value.has_value());
    CHECK(rec.ub != std::string(prism::laws::CLEAN));
    CHECK(rec.ub != std::string(prism::laws::PROVED));
    CHECK_FALSE(prism::laws::is_proof(rec.ub));
}

}  // namespace

TEST_CASE("interp ub: division by zero is INT-DIV-ZERO, not CLEAN or PROVED") {
    const std::vector<std::pair<std::string, std::string>> xy = {{"int", "x"}, {"int", "y"}};
    expect_defect(run("return x / 0;", {{"x", 1}}), "INT-DIV-ZERO", "x / 0");
    expect_defect(run("return x / y;", {{"x", 1}, {"y", 0}}, xy), "INT-DIV-ZERO", "x / y");
    expect_defect(run("return x % y;", {{"x", 1}, {"y", 0}}, xy), "INT-DIV-ZERO", "x % y");
    expect_defect(run("return x / y;", {{"x", 1}, {"y", 0}}, {{"unsigned", "x"}, {"unsigned", "y"}}), "INT-DIV-ZERO",
                  "unsigned /");
    auto pub = prism::concrete_execute(synth("return x / y;", xy), {{"x", 1}, {"y", 0}});
    CHECK(pub.ub == "INT-DIV-ZERO");
    CHECK(pub.ub != std::string(prism::laws::CLEAN));
    CHECK(pub.ub != std::string(prism::laws::PROVED));
}

TEST_CASE("interp ub: a shift amount outside [0, width) is INT-SHIFT-UB") {
    const std::vector<std::pair<std::string, std::string>> xy = {{"int", "x"}, {"int", "y"}};
    const std::vector<std::pair<std::string, std::string>> none;
    expect_defect(run("return x << y;", {{"x", 1}, {"y", -1}}, xy), "INT-SHIFT-UB", "x << -1");
    expect_defect(run("return x >> y;", {{"x", 1}, {"y", -1}}, xy), "INT-SHIFT-UB", "x >> -1");
    expect_defect(run("return 1 << -1;", {}, none), "INT-SHIFT-UB", "1 << -1");
    expect_defect(run("return 1 >> -1;", {}, none), "INT-SHIFT-UB", "1 >> -1");
    // Amount equal to the width, and greater than the width.
    expect_defect(run("return x << 32;", {{"x", 1}}), "INT-SHIFT-UB", "x << 32");
    expect_defect(run("return x >> 32;", {{"x", 1}}), "INT-SHIFT-UB", "x >> 32");
    expect_defect(run("return x << y;", {{"x", 1}, {"y", 40}}, xy), "INT-SHIFT-UB", "x << 40");
    expect_defect(run("return x >> y;", {{"x", 1}, {"y", 40}}, xy), "INT-SHIFT-UB", "x >> 40");
    expect_defect(run("return x << 32;", {{"x", 1}}, {{"unsigned", "x"}}), "INT-SHIFT-UB", "unsigned << 32");
    expect_defect(run("return x << y;", {{"x", 1}, {"y", 64}}, {{"long long", "x"}, {"int", "y"}}, "long long"),
                  "INT-SHIFT-UB", "long long << 64");
}

TEST_CASE("interp ub: 1 << 3 on a 32-bit int is 8") {
    const std::vector<std::pair<std::string, std::string>> none;
    auto lit = run("return 1 << 3;", {}, none);
    CHECK(lit.ub.empty());
    CHECK(lit.error.empty());
    CHECK(lit.value == 8);
    CHECK_FALSE(prism::laws::is_proof(lit.ub));
    auto var = run("return x << 3;", {{"x", 1}});
    CHECK(var.ub.empty());
    CHECK(var.error.empty());
    CHECK(var.value == 8);
    auto wide = run("return 1LL << 3;", {}, none, "long long");
    CHECK(wide.ub.empty());
    CHECK(wide.value == 8);
    auto pub = prism::concrete_execute(synth("return 1 << 3;", none), {});
    CHECK(pub.ub.empty());
    CHECK(pub.rc == 8);
}
