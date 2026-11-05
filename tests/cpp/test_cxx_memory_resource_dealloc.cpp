// deallocate(nullptr, 0) is a no-op. deallocate(nullptr, n) for n > 0 is
// PTR-NULL-DEREF ("memory_resource::deallocate null pointer"), then
// __builtin_trap, before any free. Reads the <memory_resource> model as text.
// This is not PROVED and not CLEAN.

#include <doctest/doctest.h>

#include "test_support.hpp"

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

// Body of the nth function whose text contains `sig`, including braces.
std::string body_of(std::string_view src, std::string_view sig, std::size_t nth = 0) {
    std::size_t from = 0;
    for (std::size_t n = 0;; ++n) {
        const auto at = src.find(sig, from);
        if (at == std::string_view::npos) return {};
        from = at + sig.size();
        if (n != nth) continue;
        const auto brace = src.find('{', at);
        if (brace == std::string_view::npos) return {};
        int depth = 0;
        for (std::size_t i = brace; i < src.size(); ++i) {
            if (src[i] == '{') ++depth;
            else if (src[i] == '}') {
                if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
            }
        }
        return {};
    }
}

std::string brace_after(std::string_view src, std::size_t at) {
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

std::size_t count_sub(std::string_view s, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = s.find(needle, at)) != std::string_view::npos; at += needle.size()) ++n;
    return n;
}

}  // namespace

TEST_CASE("cxx memory_resource model: null deallocate is a no-op or a trap before any free") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "memory_resource";
    REQUIRE(fs::is_regular_file(header));
    const auto src = slurp(header);
    REQUIRE(!src.empty());

    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("not PROVED") != std::string::npos);
    CHECK(src.find("not CLEAN") != std::string::npos);
    CHECK(src.find("does not crash") != std::string::npos);

    const auto de = body_of(src, "void deallocate(void* __p, size_t __bytes, size_t __alignment)");
    REQUIRE(!de.empty());

    const std::string zero = "if (__p == nullptr && __bytes == 0) return;";
    const std::string positive = "if (__p == nullptr && __bytes > 0)";
    const std::string check =
        "__prism_check(0, \"PTR-NULL-DEREF\", \"memory_resource::deallocate null pointer\")";
    const auto zero_at = de.find(zero);
    const auto positive_at = de.find(positive);
    const auto check_at = de.find(check);
    const auto trap_at = de.find("__builtin_trap()");
    const auto handed_at = de.find("not handed out");
    REQUIRE(zero_at != std::string::npos);
    REQUIRE(positive_at != std::string::npos);
    REQUIRE(check_at != std::string::npos);
    REQUIRE(trap_at != std::string::npos);
    REQUIRE(handed_at != std::string::npos);
    CHECK(count_sub(de, zero) == 1);
    CHECK(count_sub(de, "__builtin_trap()") == 1);
    CHECK(count_sub(src, "__builtin_trap()") == 1);
    CHECK(zero_at < positive_at);
    CHECK(positive_at < check_at);
    CHECK(check_at < trap_at);
    CHECK(trap_at < handed_at);
    CHECK(de.find("free(") == std::string::npos);
    CHECK(de.find("::operator delete") == std::string::npos);
    CHECK(prism_test::appears_before(de, "PTR-NULL-DEREF", "MEM-INVALID-FREE"));

    const auto line_end = de.find('\n', zero_at);
    REQUIRE(line_end != std::string::npos);
    const auto zero_line = de.substr(zero_at, line_end - zero_at);
    CHECK(zero_line.find("__prism_check") == std::string::npos);
    CHECK(zero_line.find("__builtin_trap") == std::string::npos);
    CHECK(zero_line.find("free(") == std::string::npos);

    const auto arm = brace_after(de, positive_at);
    REQUIRE(!arm.empty());
    const auto arm_check = arm.find(check);
    const auto arm_trap = arm.find("__builtin_trap()");
    REQUIRE(arm_check != std::string::npos);
    REQUIRE(arm_trap != std::string::npos);
    CHECK(arm_check < arm_trap);
    CHECK(arm.find("free(") == std::string::npos);
    CHECK(arm.find("::operator delete") == std::string::npos);
    CHECK(arm.find("not handed out") == std::string::npos);
    CHECK(arm.find("_M_buf") == std::string::npos);
    const auto after_trap = arm.substr(arm_trap + std::string("__builtin_trap();").size());
    for (unsigned char c : after_trap) {
        CHECK((c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '}'));
    }

    // A nonzero allocate that does not fit returns nullptr before the pointer
    // is formed and before it is recorded. That null result is not a handed-out
    // object. The allocate body is otherwise unchanged.
    const auto alloc = body_of(src, "void* allocate(size_t __bytes, size_t __alignment)");
    REQUIRE(!alloc.empty());
    const auto fits_ret = alloc.find("if (!__fits) return nullptr;");
    const auto formed = alloc.find("_M_buf +");
    const auto bump = alloc.find("_M_used = _M_used + __pad + __bytes;");
    const auto hand = alloc.find("_M_hand[_M_nhand] = __p;");
    REQUIRE(fits_ret != std::string::npos);
    REQUIRE(formed != std::string::npos);
    REQUIRE(bump != std::string::npos);
    REQUIRE(hand != std::string::npos);
    CHECK(fits_ret < formed);
    CHECK(formed < bump);
    CHECK(bump < hand);
    CHECK(alloc.find("__builtin_trap") == std::string::npos);
    CHECK(alloc.find("free(") == std::string::npos);
}
