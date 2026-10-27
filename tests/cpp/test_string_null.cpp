// string.cpp null checks name the function that contains the check.

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

TEST_CASE("string models: strcpy and memcpy null checks, strcat message unchanged") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "string.cpp");
    REQUIRE(!src.empty());

    const auto strcpy_fn = src.find("char *strcpy(");
    const auto strncpy_fn = src.find("char *strncpy(");
    REQUIRE(strcpy_fn != std::string::npos);
    REQUIRE(strncpy_fn != std::string::npos);
    REQUIRE(strcpy_fn < strncpy_fn);
    const auto strcpy_check = src.find("__prism_check(", strcpy_fn);
    REQUIRE(strcpy_check != std::string::npos);
    REQUIRE(strcpy_check < strncpy_fn);
    const auto strcpy_msg = check_message(src, strcpy_check);
    CHECK(strcpy_msg.find("strcpy()") != std::string::npos);

    const auto memcpy_fn = src.find("void *memcpy(");
    const auto memmove_fn = src.find("void *memmove(");
    REQUIRE(memcpy_fn != std::string::npos);
    REQUIRE(memmove_fn != std::string::npos);
    const auto memcpy_check = src.find("__prism_check(", memcpy_fn);
    REQUIRE(memcpy_check != std::string::npos);
    REQUIRE(memcpy_check < memmove_fn);

    const auto strcat_fn = src.find("char *strcat(");
    const auto strncat_fn = src.find("char *strncat(");
    REQUIRE(strcat_fn != std::string::npos);
    REQUIRE(strncat_fn != std::string::npos);
    const auto strcat_check = src.find("__prism_check(", strcat_fn);
    REQUIRE(strcat_check != std::string::npos);
    REQUIRE(strcat_check < strncat_fn);
    const auto strcat_msg = check_message(src, strcat_check);
    CHECK(strcat_msg == "strcat() called with unchecked pointer parameter d");
    CHECK(strcat_msg.find("strcat()") != std::string::npos);
}
