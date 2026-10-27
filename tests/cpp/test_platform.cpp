// The stage process runner (src/prism/stages/platform.cpp). It is not a
// pipeline stage: run_pipeline does not call it, and it is not in
// STAGE_ORDER. A missing executable must still be NOTRUN (Law 1), written
// down (Law 7), and never CLEAN or PROVED.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"

#include "../../src/prism/stages/common.hpp"

#ifdef ERROR
#  undef ERROR
#endif

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

TEST_CASE("platform: a missing executable is NOTRUN, never CLEAN or PROVED") {
    prism::Config cfg;
    auto missing = fs::temp_directory_path() / "prism-missing-platform-tool-f4a91c2e.exe";
    std::error_code ec;
    fs::remove(missing, ec);
    REQUIRE_FALSE(fs::exists(missing));

    auto run = prism::stages_detail::run_argv({missing.string()}, "", cfg.timeout);
    REQUIRE(run.failed);
    CHECK(run.rc == 127);
    CHECK_FALSE(run.timeout);
    CHECK_FALSE(run.crashed);

    auto found = prism::stages_detail::notrun_if_unstarted("platform", run);
    REQUIRE(found.has_value());
    CHECK(found->status == prism::laws::NOTRUN);
    CHECK(found->status != prism::laws::CLEAN);
    CHECK(found->status != prism::laws::PROVED);
    CHECK_FALSE(prism::laws::is_proof(found->status));
    CHECK_FALSE(found->message.empty());
    auto hint = found->extra.find("install");
    REQUIRE(hint != found->extra.end());
    CHECK_FALSE(hint->second.empty());
}

TEST_CASE("platform: a started process stays unclassified") {
    prism::stages_detail::ProcRun started;
    started.rc = 127;
    started.failed = false;
    started.err = "usage";
    CHECK_FALSE(prism::stages_detail::notrun_if_unstarted("platform", started).has_value());

    prism::stages_detail::ProcRun clean_exit;
    clean_exit.rc = 0;
    clean_exit.failed = false;
    auto none = prism::stages_detail::notrun_if_unstarted("platform", clean_exit);
    CHECK_FALSE(none.has_value());
}
