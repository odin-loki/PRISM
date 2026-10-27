// Every STAGE_ORDER name is a legal --stage value (src/prism/cli.cpp
// parse_cli). The span is argv after the program name, the same shape as
// tests/cpp/test_cli_stages.cpp: {"--stage", name}. An unknown name is
// CliError. Omitting --stage leaves the selection unset (every stage).

#include <doctest/doctest.h>

#include "prism/cli.hpp"
#include "prism/pipeline.hpp"

#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace {

prism::CliResult<prism::CliOptions> parse(std::initializer_list<const char*> args) {
    std::vector<const char*> v(args);
    return prism::parse_cli(std::span<const char* const>(v.data(), v.size()));
}

}  // namespace

TEST_CASE("stage order: every STAGE_ORDER name is accepted by --stage") {
    int n = 0;
    for (const char* const* p = prism::STAGE_ORDER; *p != nullptr; ++p) {
        auto r = parse({"--stage", *p});
        CHECK_MESSAGE(static_cast<bool>(r), *p);
        if (!r) continue;
        REQUIRE(r->cfg.stages);
        CHECK(*r->cfg.stages == std::vector<std::string>{*p});
        CHECK(r->cfg.want(*p));
        ++n;
    }
    CHECK(n > 0);
}

TEST_CASE("stage order: an unknown --stage name is CliError") {
    auto r = parse({"--stage", "not-a-stage"});
    CHECK_FALSE(r);
    CHECK(r.error().starts_with("argument --stage: unknown stage 'not-a-stage' (stages: "));
}

TEST_CASE("stage order: omitting --stage leaves the selection unset") {
    auto r = parse({});
    REQUIRE(r);
    CHECK_FALSE(r->cfg.stages);
    for (const char* const* p = prism::STAGE_ORDER; *p != nullptr; ++p) CHECK(r->cfg.want(*p));
}
