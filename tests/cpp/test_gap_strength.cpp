// A finding that did not prove anything is READS. prove_with_contract and
// run_wp return NEEDS-HARNESS for a POINTER function; that row is
// STRENGTH_READS and is_proof is false. pir_one's empty-result ERROR
// ("BMC produced no result") is the same strength. the pir stage does not return
// an empty list for one function without Z3 (it returns NOTRUN), so that
// make_find is checked in common.cpp. A WP obligation that holds stays
// PROVED-ASSUMING with STRENGTH_PROVES.
// docs/finish-notes/gap-strength.md

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/stages.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

prism::FunctionInfo fn_from(const std::string& stem, const std::string& src, const char* name) {
    auto dir = fs::temp_directory_path() / "prism_gap_strength";
    fs::create_directories(dir);
    auto path = dir / stem;
    {
        std::ofstream out(path, std::ios::binary);
        out << src;
    }
    for (auto& f : prism::extract_functions(path, path.string()))
        if (f.name == name) return f;
    FAIL("missing function " << name << " in " << stem);
    return {};
}

void expect_reads(const prism::Finding& f, std::string_view status) {
    CHECK(f.status == status);
    CHECK(f.strength == prism::laws::STRENGTH_READS);
    CHECK(f.strength != prism::laws::STRENGTH_PROVES);
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.status != prism::laws::PROVED);
    CHECK(f.status != prism::laws::PROVED_UNBOUNDED);
    CHECK(f.status != prism::laws::PROVED_CERTIFIED);
    CHECK(f.status != prism::laws::PROVED_ASSUMING);
}

prism::FunctionInfo pointer_fn() {
    auto f = fn_from("gap_ptr.c",
                     "int gap_ptr(int *p) {\n"
                     "    // ensures: result == 0\n"
                     "    return 0;\n"
                     "}\n",
                     "gap_ptr");
    REQUIRE(f.kind == "POINTER");
    CHECK_FALSE(prism::body_needs_pointer_harness(f.body));
    return f;
}

}  // namespace

TEST_CASE("gap strength: a POINTER contract and WP row are NEEDS-HARNESS and READS") {
    auto fn = pointer_fn();
    auto contract = prism::prove_with_contract(fn, 8, std::string("p != 0"), std::string("result == 0"));
    CHECK(contract.stage == "contracts");
    CHECK(contract.function == "gap_ptr");
    CHECK(contract.cls == "FUNC-CONTRACT");
    expect_reads(contract, prism::laws::NEEDS_HARNESS);
    CHECK(contract.message.find("POINTER") != std::string::npos);
    CHECK(contract.message.find("not a proof") != std::string::npos);

    auto rows = prism::run_wp({fn}, 8);
    REQUIRE(rows.size() == 1);
    const auto& wp = rows[0];
    CHECK(wp.stage == "wp");
    CHECK(wp.function == "gap_ptr");
    CHECK(wp.cls == "FUNC-CONTRACT");
    expect_reads(wp, prism::laws::NEEDS_HARNESS);
    CHECK(wp.message.find("POINTER") != std::string::npos);
    CHECK(wp.message.find("not a proof") != std::string::npos);
    CHECK(wp.extra.at("engine") == "prism-wp");
}

TEST_CASE("gap strength: WP scalar-subset, scrubbed byte, and unencoded predicate are READS") {
    auto void_fn = fn_from("gap_void.c",
                           "void gap_void(void) {\n"
                           "    // ensures: result == 0\n"
                           "    return;\n"
                           "}\n",
                           "gap_void");
    REQUIRE(void_fn.kind == "VOID");
    auto void_rows = prism::run_wp({void_fn}, 4);
    REQUIRE(void_rows.size() == 1);
    expect_reads(void_rows[0], prism::laws::NEEDS_HARNESS);
    CHECK(void_rows[0].message.find("VOID") != std::string::npos);
    CHECK(void_rows[0].message.find("not a proof") != std::string::npos);

    auto scrub = fn_from("gap_scrub.c",
                         "int gap_scrub(int x) {\n"
                         "    // ensures: result == x\n"
                         "    return x;\n"
                         "}\n",
                         "gap_scrub");
    REQUIRE(scrub.kind == "SCALAR");
    scrub.body.push_back(prism::SCRUBBED_BYTE);
    auto scrub_rows = prism::run_wp({scrub}, 4);
    REQUIRE(scrub_rows.size() == 1);
    expect_reads(scrub_rows[0], prism::laws::NEEDS_HARNESS);
    CHECK(scrub_rows[0].message.find("not a proof") != std::string::npos);
    CHECK(scrub_rows[0].message.find("WP") != std::string::npos);

    auto bad = fn_from("gap_unenc.c",
                       "int gap_unenc(int x) {\n"
                       "    // ensures: \\valid(&x)\n"
                       "    return x;\n"
                       "}\n",
                       "gap_unenc");
    REQUIRE(bad.kind == "SCALAR");
    auto bad_rows = prism::run_wp({bad}, 4);
    REQUIRE(bad_rows.size() == 1);
    expect_reads(bad_rows[0], prism::laws::ERROR);
    CHECK(bad_rows[0].message.find("cannot encode WP predicate") != std::string::npos);
    CHECK(bad_rows[0].extra.at("wp_unencoded") == "true");
}

TEST_CASE("gap strength: contracts fail() on an unencodable decreases is ERROR and READS") {
    auto fn = fn_from("gap_dec.c",
                      "int gap_dec(int n) {\n"
                      "    // decreases: n * 2\n"
                      "    return n;\n"
                      "}\n",
                      "gap_dec");
    REQUIRE(fn.kind == "SCALAR");
    CHECK_FALSE(prism::body_needs_pointer_harness(fn.body));
    auto rows = prism::prove_contracts({fn}, 8);
    REQUIRE(rows.size() == 1);
    expect_reads(rows[0], prism::laws::ERROR);
    CHECK(rows[0].stage == "contracts");
    CHECK(rows[0].message.find("cannot encode decreases") != std::string::npos);
    CHECK(rows[0].extra.at("decreases_unencoded") == "true");
}

TEST_CASE("gap strength: a holding WP stays PROVED-ASSUMING with PROVES") {
    auto path = fs::path(PRISM_SOURCE_DIR) / "testdata" / "planted" / "contract_add.c";
    prism::FunctionInfo inc;
    bool found = false;
    for (auto& f : prism::extract_functions(path, path.string())) {
        if (f.name == "inc") {
            inc = f;
            found = true;
        }
    }
    REQUIRE(found);
    REQUIRE(inc.kind == "SCALAR");
    auto rows = prism::run_wp({inc}, 8);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].status == prism::laws::PROVED_ASSUMING);
    CHECK(rows[0].strength == prism::laws::STRENGTH_PROVES);
    CHECK(prism::laws::is_proof(rows[0].status));
}
