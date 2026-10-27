// The source_location model is one extensionless file. It records the call
// with compiler builtins. CMake's prism_tests source list is explicit (not a glob).

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

TEST_CASE("cxx source_location header uses compiler location builtins") {
    const fs::path header =
        fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "source_location";
    REQUIRE(fs::is_regular_file(header));
    std::ifstream in(header);
    REQUIRE(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    CHECK(text.find("__builtin_FILE") != std::string::npos);
    CHECK(text.find("__builtin_LINE") != std::string::npos);
    CHECK(text.find("unknown") == std::string::npos);
}
