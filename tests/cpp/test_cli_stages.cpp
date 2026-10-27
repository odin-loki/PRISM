// Stage selection on `prism PATH` (src/prism/cli.cpp parse_cli).
// Law 8: no flag silently disables a check. Law 7: a stage that cannot
// run is written down; an unknown name is an error, not a quiet skip.
// main returns 2 when parse_cli yields CliError.

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

std::string error_of(std::initializer_list<const char*> args) {
    auto r = parse(args);
    CHECK_FALSE(r);
    return r ? std::string() : r.error();
}

void check_every_stage(const prism::Config& cfg) {
    CHECK_FALSE(cfg.stages);
    for (const char* const* p = prism::STAGE_ORDER; *p; ++p) CHECK(cfg.want(*p));
}

}  // namespace

TEST_CASE("cli stages: omitting --stage selects every stage") {
    auto bare = parse({});
    REQUIRE(bare);
    check_every_stage(bare->cfg);

    // An empty selection is every stage, not none (an engaged empty list
    // would make want() false for the whole STAGE_ORDER).
    for (const char* empty : {"", " , "}) {
        auto r = parse({"--stage", empty});
        REQUIRE(r);
        check_every_stage(r->cfg);
    }
    auto joined = parse({"--stage="});
    REQUIRE(joined);
    check_every_stage(joined->cfg);
}

TEST_CASE("cli stages: an unknown stage name is CliError") {
    CHECK(error_of({"--stage", "not-a-stage"})
              .starts_with("argument --stage: unknown stage 'not-a-stage' (stages: "));
    CHECK(error_of({"--stage", "bmc", "--stage", "nope"})
              .starts_with("argument --stage: unknown stage 'nope' (stages: "));
    CHECK(error_of({"--stage=bmc,,nope"}).starts_with("argument --stage: unknown stage 'nope' (stages: "));
}

TEST_CASE("cli stages: repeating --stage accumulates") {
    auto r = parse({"--stage", "bmc", "--stage", "pir", "--stage=fuzz"});
    REQUIRE(r);
    REQUIRE(r->cfg.stages);
    CHECK(*r->cfg.stages == std::vector<std::string>{"bmc", "pir", "fuzz"});
    CHECK(r->cfg.want("bmc"));
    CHECK(r->cfg.want("pir"));
    CHECK(r->cfg.want("fuzz"));
    CHECK_FALSE(r->cfg.want("inventory"));

    auto csv = parse({"--stage", "bmc,pir", "--stage", "harness"});
    REQUIRE(csv);
    REQUIRE(csv->cfg.stages);
    CHECK(*csv->cfg.stages == std::vector<std::string>{"bmc", "pir", "harness"});

    // A later empty --stage keeps the names already chosen. `--stage=` is
    // the same empty value: only bmc, not every stage.
    auto kept = parse({"--stage", "bmc", "--stage", ""});
    REQUIRE(kept);
    REQUIRE(kept->cfg.stages);
    CHECK(*kept->cfg.stages == std::vector<std::string>{"bmc"});
    CHECK(kept->cfg.want("bmc"));
    CHECK_FALSE(kept->cfg.want("pir"));

    auto joined = parse({"--stage", "bmc", "--stage="});
    REQUIRE(joined);
    REQUIRE(joined->cfg.stages);
    CHECK(*joined->cfg.stages == std::vector<std::string>{"bmc"});
    CHECK(joined->cfg.want("bmc"));
    CHECK_FALSE(joined->cfg.want("pir"));
    CHECK_FALSE(joined->cfg.want("inventory"));
}

TEST_CASE("cli skip: repeating --skip accumulates and an empty --skip keeps names") {
    auto both = parse({"--skip", "bmc", "--skip", "pir"});
    REQUIRE(both);
    CHECK(both->cfg.skip == std::vector<std::string>{"bmc", "pir"});
    CHECK_FALSE(both->cfg.stages);
    CHECK_FALSE(both->cfg.want("bmc"));
    CHECK_FALSE(both->cfg.want("pir"));
    CHECK(both->cfg.want("inventory"));

    // A later empty --skip= does not wipe names already skipped.
    auto kept = parse({"--skip", "bmc", "--skip="});
    REQUIRE(kept);
    CHECK(kept->cfg.skip == std::vector<std::string>{"bmc"});
    CHECK_FALSE(kept->cfg.want("bmc"));
    CHECK(kept->cfg.want("pir"));
}

TEST_CASE("cli skip: an unknown skip name is CliError") {
    CHECK(error_of({"--skip", "not-a-stage"})
              .starts_with("argument --skip: unknown stage 'not-a-stage' (stages: "));
    CHECK(error_of({"--skip", "bmc", "--skip", "nope"})
              .starts_with("argument --skip: unknown stage 'nope' (stages: "));
    CHECK(error_of({"--skip=bmc,,nope"}).starts_with("argument --skip: unknown stage 'nope' (stages: "));
}

TEST_CASE("cli stages: --no-bmc, --skip-stage and --disable are CliError") {
    CHECK(error_of({"--no-bmc"}) == "unrecognized arguments: --no-bmc");
    CHECK(error_of({"--skip-stage"}) == "unrecognized arguments: --skip-stage");
    CHECK(error_of({"--disable"}) == "unrecognized arguments: --disable");
}
