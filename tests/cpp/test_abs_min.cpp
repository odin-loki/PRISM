// abs, labs, and llabs: the type minimum is a contract check then a trap.
// Reads stdlib.cpp as text.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

std::string function_body(std::string_view src, std::string_view sig) {
    const auto at = src.find(sig);
    if (at == std::string_view::npos) return {};
    const auto open = src.find('{', at);
    if (open == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{') {
            ++depth;
        } else if (src[i] == '}') {
            --depth;
            if (depth == 0) return std::string(src.substr(open, i - open + 1));
        }
    }
    return {};
}

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

void check_abs(std::string_view src, std::string_view sig, std::string_view min_name, std::string_view message) {
    const auto at = src.find(sig);
    REQUIRE(at != std::string_view::npos);
    const auto body = function_body(src, sig);
    REQUIRE(!body.empty());

    const std::string cond = std::string("if (x == ") + std::string(min_name) + ")";
    const auto min_if = body.find(cond);
    REQUIRE(min_if != std::string::npos);
    const auto arm = brace_block_after(body, min_if);
    REQUIRE(!arm.empty());
    const auto check = arm.find("__prism_check(0, \"FUNC-CONTRACT\", \"");
    const auto trap = arm.find("__builtin_trap()");
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    CHECK(check < trap);
    CHECK(arm.find(message) != std::string::npos);
    // The class string "FUNC-CONTRACT" has a hyphen; the arm must not negate x.
    CHECK(arm.find("-x") == std::string::npos);
    CHECK(arm.find("- x") == std::string::npos);
    CHECK(arm.find("return") == std::string::npos);

    const auto neg = body.find("return -x;");
    const auto same = body.find("return x;");
    const auto trap_at = body.find("__builtin_trap()");
    const auto neg_if = body.find("else if (x < 0)");
    REQUIRE(neg != std::string::npos);
    REQUIRE(same != std::string::npos);
    REQUIRE(trap_at != std::string::npos);
    REQUIRE(neg_if != std::string::npos);
    CHECK(min_if < neg_if);
    CHECK(neg_if < neg);
    CHECK(neg < same);
    CHECK(trap_at < neg);
    CHECK(body.find("return 0") == std::string::npos);
    CHECK(body.find("? -") == std::string::npos);
}

}  // namespace

TEST_CASE("stdlib model: abs of the type minimum is a contract check then a trap") {
    const auto src = slurp(std::filesystem::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" /
                           "libc" / "stdlib.cpp");
    REQUIRE(!src.empty());
    CHECK(src.find("#include <limits.h>") != std::string::npos);

    const auto abs_at = src.find("int abs(int x)");
    REQUIRE(abs_at != std::string::npos);
    const auto note_at = src.rfind("/*", abs_at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, abs_at - note_at);
    CHECK(note.find("INT_MIN") != std::string::npos);
    CHECK(note.find("LONG_MIN") != std::string::npos);
    CHECK(note.find("LLONG_MIN") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);
    CHECK(note.find("not negated") != std::string::npos);

    check_abs(src, "int abs(int x)", "INT_MIN", "abs(INT_MIN) is undefined");
    check_abs(src, "long labs(long x)", "LONG_MIN", "labs(LONG_MIN) is undefined");
    check_abs(src, "long long llabs(long long x)", "LLONG_MIN", "llabs(LLONG_MIN) is undefined");

    CHECK(src.find("the seed is not stored") != std::string::npos);
    const auto bsearch = function_body(src, "void *bsearch(");
    REQUIRE(!bsearch.empty());
    const auto assume = bsearch.find("__prism_assume(0)");
    REQUIRE(assume != std::string::npos);
    CHECK(bsearch.find("return 0;", assume) != std::string::npos);
}
