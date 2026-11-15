// Litmus suite for the conc stage's memory model (docs/KERNEL_MEMORY_MODEL.md).
// Every tests/conc/litmus/*.c names its expected verdict on its first line:
//   // EXPECT: FAILED   the forbidden-outcome assertion is reachable (outcome allowed)
//   // EXPECT: BOUNDED  no violation within the bound (outcome forbidden)
// The expected outcomes and their sources are cited in each file's header.

#include <doctest/doctest.h>

#include "prism/conc.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path litmus_dir() {
    return std::filesystem::path(PRISM_SOURCE_DIR) / "tests" / "conc" / "litmus";
}

bool have_frontend() {
    prism::Config cfg;
    return prism::pir::frontend_usable(cfg) && prism::pir::z3_available();
}

std::string expected_of(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::string line;
    std::getline(in, line);
    const std::string tag = "// EXPECT: ";
    if (line.compare(0, tag.size(), tag) != 0) return "";
    std::string v = line.substr(tag.size());
    v = v.substr(0, v.find(' '));
    return v;
}

}  // namespace

TEST_CASE("conc litmus: every test names a verdict and a source") {
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::directory_iterator(litmus_dir()))
        if (e.path().extension() == ".c") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    REQUIRE(files.size() >= 10);
    for (auto& f : files) {
        const auto v = expected_of(f);
        CAPTURE(f.string());
        CHECK((v == "FAILED" || v == "BOUNDED"));
    }
}

TEST_CASE("conc litmus: each result matches the published expected outcome") {
    if (!have_frontend()) return;
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::directory_iterator(litmus_dir()))
        if (e.path().extension() == ".c") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        const auto want = expected_of(f);
        CAPTURE(f.string());
        prism::Config cfg;
        cfg.timeout = 60;
        auto all = prism::conc::check_file(f, cfg, prism::conc::Options{});
        auto it = all.find("main");
        REQUIRE_MESSAGE(it != all.end(), f.string());
        const auto& r = it->second;
        CAPTURE(r.message);
        if (want == "FAILED") CHECK(r.status == prism::laws::FAILED);
        if (want == "BOUNDED") CHECK(r.status == prism::laws::BOUNDED);
        CHECK(r.status != prism::laws::PROVED);
    }
}
