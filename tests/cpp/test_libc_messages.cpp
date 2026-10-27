// Libc model __prism_check text names the function that contains the check.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Third string literal of the __prism_check call that starts at `check`.
std::string check_message(std::string_view src, std::size_t check) {
    int quotes = 0;
    std::size_t start = 0;
    for (std::size_t i = check; i < src.size(); ++i) {
        if (src[i] != '"') continue;
        ++quotes;
        if (quotes == 3) start = i + 1;
        if (quotes == 4) return std::string(src.substr(start, i - start));
    }
    return {};
}

}  // namespace

TEST_CASE("libc models: strdup null-check message names strdup") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "string.cpp");
    REQUIRE(!src.empty());
    const auto fn = src.find("char *strdup(");
    REQUIRE(fn != std::string::npos);
    const auto check = src.find("__prism_check(", fn);
    REQUIRE(check != std::string::npos);
    const auto call_end = src.find(");", check);
    REQUIRE(call_end != std::string::npos);
    const auto call = src.substr(check, call_end - check);
    CHECK(call.find("s != 0") != std::string::npos);
    CHECK(call.find("STR-NULL-ARG") != std::string::npos);
    const auto msg = check_message(src, check);
    CHECK(msg.find("strdup") != std::string::npos);
    CHECK(msg.find("strlen() called") == std::string::npos);
}
