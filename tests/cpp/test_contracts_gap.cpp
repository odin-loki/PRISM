// Law 6: a pointer parameter is not model-checked unguarded.
// Law 7: a function that has a contract and cannot be checked is written
// down. prove_contracts (src/prism/stages/contracts.cpp) returns
// NEEDS-HARNESS before BMC for a POINTER function. A SCALAR contract
// reaches bmc_one; without Z3 that row is NOTRUN, never PROVED and never
// dropped. Linked into prism_tests next to test_main.cpp (which provides
// main()). The pointer-parameter case runs on every build.
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
#include <vector>

namespace {

std::vector<prism::FunctionInfo> parse_source(const std::string& name, const std::string& src) {
    auto dir = std::filesystem::temp_directory_path() / "prism_contracts_gap";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    {
        std::ofstream out(path, std::ios::binary);
        out << src;
    }
    return prism::extract_functions(path, path.string());
}

prism::FunctionInfo must_fn(std::vector<prism::FunctionInfo> fns, const char* name) {
    for (auto& f : fns)
        if (f.name == name) return f;
    FAIL("missing function " << name);
    return {};
}

void require_not_proof(const prism::Finding& f) {
    CHECK(f.status != std::string(prism::laws::PROVED));
    CHECK(f.status != std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK(f.status != std::string(prism::laws::PROVED_ASSUMING));
    CHECK(f.status != std::string(prism::laws::PROVED_CERTIFIED));
    CHECK(f.status != std::string(prism::laws::CLEAN));
    CHECK_FALSE(prism::laws::is_proof(f.status));
}

}  // namespace

TEST_CASE("contracts: a pointer parameter with a contract is NEEDS-HARNESS") {
    // The body does not allocate or declare a pointer. The only Law 6 reason
    // is the parameter. ptr_plain has no contract, so the stage writes no
    // row for it. Not main, so argv is not rewritten away.
    auto contracted = must_fn(parse_source("ptr_contract.c",
                                           "int ptr_contract(int *p) {\n"
                                           "    // requires: p != 0\n"
                                           "    // ensures: result == 0\n"
                                           "    return 0;\n"
                                           "}\n"),
                              "ptr_contract");
    auto plain = must_fn(parse_source("ptr_plain.c", "int ptr_plain(int *q) { return 0; }\n"), "ptr_plain");
    CHECK(contracted.kind == "POINTER");
    CHECK(plain.kind == "POINTER");
    REQUIRE(contracted.params.size() == 1);
    CHECK(contracted.params[0].second == "p");
    CHECK(contracted.params[0].first.find('*') != std::string::npos);
    CHECK_FALSE(prism::body_needs_pointer_harness(contracted.body));

    auto rows = prism::prove_contracts({contracted, plain}, 8);
    REQUIRE(rows.size() == 1);
    const auto& f = rows[0];
    CHECK(f.function == "ptr_contract");
    CHECK(f.stage == "contracts");
    CHECK(f.cls == "FUNC-CONTRACT");
    CHECK(f.status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(f.status != std::string(prism::laws::NOTRUN));
    require_not_proof(f);
    CHECK(f.message.find("POINTER") != std::string::npos);
    CHECK(f.message.find("invent a buffer") != std::string::npos);
    CHECK(f.message.find("not a proof") != std::string::npos);
    REQUIRE(f.extra.contains("requires"));
    REQUIRE(f.extra.contains("ensures"));
    CHECK(f.extra.at("requires").find("p != 0") != std::string::npos);
    CHECK(f.extra.at("ensures").find("result == 0") != std::string::npos);
}

#ifndef PRISM_HAS_Z3
TEST_CASE("contracts: missing Z3 on a contracted scalar is NOTRUN, never PROVED or empty") {
    // SCALAR, no pointer in the body: the stage must call the backend.
    // bare has no contract, so an empty list would hide the gap on held.
    auto held = must_fn(parse_source("held_contract.c",
                                     "int held(int x) {\n"
                                     "    // requires: x >= 0\n"
                                     "    // ensures: result == x\n"
                                     "    return x;\n"
                                     "}\n"),
                        "held");
    auto bare = must_fn(parse_source("bare_contract.c", "int bare(int y) { return y; }\n"), "bare");
    CHECK(held.kind == "SCALAR");
    CHECK(bare.kind == "SCALAR");
    REQUIRE(held.params.size() == 1);
    CHECK(held.params[0].first.find('*') == std::string::npos);
    CHECK_FALSE(prism::body_needs_pointer_harness(held.body));

    auto rows = prism::prove_contracts({held, bare}, 8);
    REQUIRE(rows.size() == 1);
    const auto& f = rows[0];
    CHECK(f.function == "held");
    CHECK(f.stage == "contracts");
    CHECK(f.cls == "FUNC-CONTRACT");
    CHECK(f.status == std::string(prism::laws::NOTRUN));
    require_not_proof(f);
    CHECK(f.message.find("z3") != std::string::npos);
    REQUIRE(f.extra.contains("install"));
    CHECK(f.extra.at("install").find("PRISM_Z3") != std::string::npos);
    REQUIRE(f.extra.contains("original_status"));
    CHECK(f.extra.at("original_status") == std::string(prism::laws::NOTRUN));
    REQUIRE(f.extra.contains("requires"));
    REQUIRE(f.extra.contains("ensures"));
    CHECK(f.extra.at("requires").find("x >= 0") != std::string::npos);
    CHECK(f.extra.at("ensures").find("result == x") != std::string::npos);
}
#else
TEST_CASE("contracts: missing Z3 on a contracted scalar is NOTRUN, never PROVED or empty") {
    // The missing-backend row is run_bmc's build without Z3 ("z3 not built").
    // With Z3 the same scalar contract is a solver answer, not this gap.
    MESSAGE("PRISM_HAS_Z3: skip missing-backend NOTRUN row");
}
#endif
