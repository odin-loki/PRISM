// Law 6: a function with a pointer parameter is not model-checked unguarded.
// run_bmc (include/prism/stages.hpp) is the public entry. With
// allow_local_pointers false the finding is NEEDS-HARNESS. The header
// default for the data model stays LP64; this case passes that model.
// Linked into prism_tests next to test_main.cpp (which provides main()).
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
    auto dir = std::filesystem::temp_directory_path() / "prism_bmc_harness";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    {
        std::ofstream out(path, std::ios::binary);
        out << src;
    }
    return prism::extract_functions(path, path.string());
}

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("bmc: a pointer parameter is NEEDS-HARNESS when allow_local_pointers is false") {
    // Body does not allocate or declare a pointer. The only Law 6 reason is
    // the parameter itself. Not main, so argv is not rewritten away.
    auto fns = parse_source("ptr_param.c", "int ptr_param(int *p) { return 0; }\n");
    REQUIRE(fns.size() == 1);
    CHECK(fns[0].name == "ptr_param");
    CHECK(fns[0].kind == "POINTER");
    REQUIRE(fns[0].params.size() == 1);
    CHECK(fns[0].params[0].second == "p");
    CHECK(fns[0].params[0].first.find('*') != std::string::npos);

    auto rows = prism::run_bmc(fns, 8, false, "LP64");
    REQUIRE(rows.size() == 1);
    const auto& f = rows[0];
    CHECK(f.function == "ptr_param");
    CHECK(f.stage == "bmc");
    CHECK(f.status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(f.status != std::string(prism::laws::PROVED));
    CHECK(f.status != std::string(prism::laws::CLEAN));
    CHECK(f.status != std::string(prism::laws::NOTRUN));
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.message.find("pointer parameter") != std::string::npos);
}
#else
TEST_CASE("bmc: a pointer parameter is NEEDS-HARNESS when allow_local_pointers is false") {
    // The pointer-parameter gate lives in the Z3 build of run_bmc. A build
    // without Z3 is Law 1 (missing tool), locked by "bmc: missing Z3 is NOTRUN".
    MESSAGE("PRISM_HAS_Z3: skip pointer-parameter NEEDS-HARNESS row");
}
#endif
