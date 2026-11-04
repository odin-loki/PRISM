// Every PRISM_* environment variable read in src/ is in prism::env_vars().
#include "prism/config.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

TEST_CASE("every PRISM_* getenv is documented in env_vars()") {
    std::set<std::string> listed;
    for (const auto& e : prism::env_vars()) {
        CHECK_FALSE(e.purpose.empty());
        CHECK(listed.insert(std::string(e.name)).second);
    }
    const std::filesystem::path src = std::filesystem::path(PRISM_SOURCE_ROOT) / "src";
    const std::regex re(R"re((?:getenv|env_or)\("(PRISM_[A-Z0-9_]+)")re");
    for (const auto& f : std::filesystem::recursive_directory_iterator(src)) {
        if (!f.is_regular_file()) continue;
        std::ifstream in(f.path());
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
            INFO(f.path().string());
            CHECK(listed.contains((*it)[1].str()));
        }
    }
}
