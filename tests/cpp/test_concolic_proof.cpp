// Law 3: a finished concolic run with no UB is CLEAN, and that row is not a
// proof. run_concolic (include/prism/stages.hpp) is the public entry.
// The FunctionInfo is filled the same way as the hand-built case in
// tests/cpp/test_concolic.cpp. No branch, so the neighbour loop never asks
// the solver; budget 1 is the smallest neighbour budget.
// Linked into prism_tests next to test_main.cpp (which provides main()).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/stages.hpp"

#include <string>

TEST_CASE("concolic: a finished scalar run with no UB is CLEAN and not a proof") {
    prism::FunctionInfo fn;
    fn.kind = "SCALAR";
    fn.name = "scalar_ok";
    fn.return_type = "int";
    fn.signature = "int scalar_ok(int x)";
    fn.line = 1;
    fn.params = {{"int", "x"}};
    fn.body = "return x;";

    auto rows = prism::run_concolic({fn}, 1);
    REQUIRE(rows.size() == 1);
    const auto& f = rows[0];
    CHECK(f.function == "scalar_ok");
    CHECK(f.stage == "concolic");
    CHECK(f.status == std::string(prism::laws::CLEAN));
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.message.find("not a proof") != std::string::npos);
}
