// Repository tooling that used to be scripts: the PLAN.md "done" smoke.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

fs::path tools_tmp(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("prism-repo-tools-" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

std::vector<std::string> statuses(const prism::RunReport& r, const std::string& stage) {
    std::vector<std::string> out;
    for (const auto& s : r.stages)
        if (s.name == stage)
            for (const auto& f : s.findings) out.push_back(f.status);
    return out;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

prism::RunReport plan_done_run(const std::string& plant, const fs::path& out) {
    prism::Config cfg = prism::default_config();
    cfg.root = repo_root() / "testdata" / plant;
    cfg.out = out;
    cfg.llm = false;
    cfg.jobs = 4;
    cfg.skip = {"llm", "optional", "repair", "execute"};
    return prism::run_pipeline(cfg);
}

}  // namespace

// docs/PLAN.md "done": prove a SCALAR, crash a plant, name a missing tool.
// Without --allow-exec (Law 9) the crash comes from the in-process
// interpreter paths (concolic / fuzz), not from running compiled code.
TEST_CASE("plan done: abs_ok PROVED-UNBOUNDED; oob_write CRASH; missing ESBMC is never a proof") {
    auto tmp = tools_tmp("plan-done");
    REQUIRE(fs::is_regular_file(repo_root() / "testdata" / "abs_ok.c"));
    auto abs = plan_done_run("abs_ok.c", tmp / "abs_ok");
    CHECK(has(statuses(abs, "bmc"), std::string(prism::laws::PROVED_UNBOUNDED)));
    // ESBMC is optional: when it is missing it is NOTRUN, never a clean or proved row (Law 1).
    for (const auto& s : statuses(abs, "esbmc")) {
        CHECK(s != prism::laws::CLEAN);
        CHECK(s != prism::laws::PROVED);
        CHECK(s != prism::laws::PROVED_UNBOUNDED);
        CHECK(s != prism::laws::BOUNDED);
    }
    auto oob = plan_done_run("oob_write.c", tmp / "oob_write");
    std::vector<std::string> dyn;
    for (const char* st : {"fuzz", "fuse", "concolic"})
        for (auto& s : statuses(oob, st)) dyn.push_back(s);
    CHECK(has(dyn, std::string(prism::laws::CRASH)));
}
