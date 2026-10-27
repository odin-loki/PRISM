// srand stores no seed. rand is nondet. Read stdlib.cpp as text.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string function_body(std::string_view src, std::size_t sig) {
    const auto open = src.find('{', sig);
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

bool is_ident(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// srand's body declares no local. `static`, or an assignment whose left-hand
// side is not the parameter `s`, stores the seed in a static or a global.
// Comparisons (`==`, `!=`, `<=`, `>=`) are not stores.
bool assigns_static_or_global(std::string_view body) {
    for (std::size_t i = 0; i + 6 <= body.size(); ++i) {
        if (body.substr(i, 6) != "static") continue;
        const bool left = i == 0 || !is_ident(body[i - 1]);
        const bool right = i + 6 == body.size() || !is_ident(body[i + 6]);
        if (left && right) return true;
    }
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '=') continue;
        const char prev = i == 0 ? '\0' : body[i - 1];
        const char next = i + 1 == body.size() ? '\0' : body[i + 1];
        if (prev == '=' || prev == '!' || prev == '<' || prev == '>') continue;
        if (next == '=') continue;
        std::size_t end = i;
        while (end > 0 && (body[end - 1] == ' ' || body[end - 1] == '\t' || body[end - 1] == '\n' ||
                           body[end - 1] == '\r'))
            --end;
        std::size_t begin = end;
        while (begin > 0 && is_ident(body[begin - 1])) --begin;
        if (begin == end) return true;
        if (body.substr(begin, end - begin) != "s") return true;
    }
    return false;
}

}  // namespace

TEST_CASE("stdlib model: srand stores no seed and rand is nondet") {
    const auto src = slurp(std::filesystem::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" /
                           "models" / "libc" / "stdlib.cpp");
    REQUIRE(!src.empty());

    const auto anchor = src.find("unsigned long strtoul(");
    const auto rand_at = src.find("int rand(void)");
    const auto srand_at = src.find("void srand(");
    const auto getenv_at = src.find("char *getenv(");
    REQUIRE(anchor != std::string::npos);
    REQUIRE(rand_at != std::string::npos);
    REQUIRE(srand_at != std::string::npos);
    REQUIRE(getenv_at != std::string::npos);
    REQUIRE(anchor < rand_at);
    REQUIRE(rand_at < srand_at);
    REQUIRE(srand_at < getenv_at);

    const auto note = src.substr(anchor, rand_at - anchor);
    CHECK(note.find("the seed is not stored") != std::string::npos);
    CHECK(note.find("not a reproducible sequence") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);

    const auto srand_body = function_body(src, srand_at);
    REQUIRE(!srand_body.empty());
    CHECK_FALSE(assigns_static_or_global(srand_body));

    const auto rand_body = function_body(src, rand_at);
    REQUIRE(!rand_body.empty());
    CHECK(rand_body.find("__VERIFIER_nondet_int") != std::string::npos);
}
