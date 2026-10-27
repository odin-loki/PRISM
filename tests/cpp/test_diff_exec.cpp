// Differential testing compiles and runs both functions (Law 9). With the
// process-wide exec policy off, run_diff must write one NOTRUN row
// (sandbox::exec_notrun), never CLEAN and never an empty list. A missing
// gcc/clang is a different NOTRUN; this case does not look at PATH.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/sandbox.hpp"
#include "prism/stages.hpp"

#include <filesystem>
#include <string>

namespace {

prism::FunctionInfo scalar(const std::string& name, const std::string& body) {
    prism::FunctionInfo f;
    f.file = "pair.c";
    f.name = name;
    f.kind = "SCALAR";
    f.line = 4;
    f.signature = "int " + name + "(int x)";
    f.params = {{"int", "x"}};
    f.return_type = "int";
    f.body = body;
    f.span = {4, 6};
    return f;
}

}  // namespace

TEST_CASE("diff exec: two SCALAR functions are NOTRUN when allow-exec is off") {
    // sandbox::allowed() is the process-global Config sets for stages that
    // take no Config. Policy puts it back when the case ends, including
    // when a REQUIRE fails.
    const bool prior = prism::sandbox::allowed();
    {
        prism::sandbox::Policy off(false);
        CHECK_FALSE(prism::sandbox::allowed());

        auto recs = prism::run_diff(
            {scalar("add_a", "    return x + 1;"), scalar("add_b", "    return x + 1;")},
            std::filesystem::path("unused"));
        REQUIRE(recs.size() == 1);
        const auto& r = recs[0];
        CHECK(r.stage == "diff");
        CHECK(r.status == prism::laws::NOTRUN);
        CHECK(r.status != prism::laws::CLEAN);
        CHECK_FALSE(prism::laws::is_proof(r.status));
        CHECK(r.file == "pair.c");
        REQUIRE(r.function.has_value());
        CHECK(*r.function == "add_a/add_b");
        CHECK(r.line == 4);
        CHECK(r.message == prism::sandbox::exec_message("diff add_a/add_b"));
        CHECK(r.message.find("no gcc/clang") == std::string::npos);
        auto reason = r.extra.find("reason");
        REQUIRE(reason != r.extra.end());
        CHECK(reason->second == prism::sandbox::EXEC_REASON);
        auto install = r.extra.find("install");
        REQUIRE(install != r.extra.end());
        CHECK(install->second == prism::sandbox::EXEC_INSTALL);
    }
    CHECK(prism::sandbox::allowed() == prior);
}
