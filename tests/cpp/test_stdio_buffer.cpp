// stdio buffer models: a null fgets buffer and an overflowing fread are checks.
// Reads stdio.cpp as text. Does not compile the model.

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

std::string function_body(std::string_view src, std::string_view sig, std::string_view next) {
    const auto begin = src.find(sig);
    if (begin == std::string_view::npos) return {};
    const auto end = src.find(next, begin + sig.size());
    if (end == std::string_view::npos) return {};
    return std::string(src.substr(begin, end - begin));
}

// Brace block that opens after `at`.
std::string brace_block_after(std::string_view src, std::size_t at) {
    const auto open = src.find('{', at);
    if (open == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}') {
            --depth;
            if (depth == 0) return std::string(src.substr(open, i + 1 - open));
        }
    }
    return {};
}

}  // namespace

TEST_CASE("stdio buffer: fgets check names fgets") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    REQUIRE(!src.empty());
    const auto body = function_body(src, "char *fgets(", "size_t fread(");
    REQUIRE(!body.empty());
    const auto check = body.find("__prism_check(");
    REQUIRE(check != std::string::npos);
    CHECK(body.find("STR-NULL-ARG") != std::string::npos);
    const auto msg = check_message(body, check);
    CHECK(msg.find("fgets") != std::string::npos);
}

TEST_CASE("stdio buffer: fread overflow is a check and does not return 0") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    REQUIRE(!src.empty());
    const auto body = function_body(src, "size_t fread(", "size_t fwrite(");
    REQUIRE(!body.empty());
    const auto mul = body.find("__builtin_mul_overflow");
    REQUIRE(mul != std::string::npos);
    const auto arm = brace_block_after(body, mul);
    REQUIRE(!arm.empty());
    CHECK(arm.find("__prism_check(") != std::string::npos);
    CHECK(arm.find("MEM-OOB-WRITE") != std::string::npos || arm.find("MEM-OOB-READ") != std::string::npos);
    CHECK(arm.find("return 0") == std::string::npos);
}

TEST_CASE("stdio buffer: fopen message still names fopen") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    REQUIRE(!src.empty());
    const auto body = function_body(src, "FILE *fopen(", "int fclose(");
    REQUIRE(!body.empty());
    int found = 0;
    for (std::size_t at = 0;;) {
        const auto check = body.find("__prism_check(", at);
        if (check == std::string::npos) break;
        const auto msg = check_message(body, check);
        CHECK(msg.find("fopen") != std::string::npos);
        ++found;
        at = check + std::string("__prism_check(").size();
    }
    CHECK(found == 2);
}
