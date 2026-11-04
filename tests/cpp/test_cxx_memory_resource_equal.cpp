// memory_resource::is_equal returns true for the same object and false for a
// different object. Neither path allocates or frees. allocate and deallocate
// are unchanged: deallocate(nullptr, 0) is a no-op, and deallocate(nullptr, n)
// for n > 0 is PTR-NULL-DEREF then __builtin_trap before any free. Reads the
// <memory_resource> model as text. This is not PROVED and not CLEAN.

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

TEST_CASE("cxx memory_resource model: is_equal is object identity and does not allocate or free") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "memory_resource";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const auto src = slurp(header);
    REQUIRE(!src.empty());

    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <cstddef>") != std::string::npos);
    CHECK(src.find("not PROVED") != std::string::npos);
    CHECK(src.find("not CLEAN") != std::string::npos);
    CHECK(src.find("class polymorphic_allocator") == std::string::npos);
    CHECK(src.find("do_is_equal") != std::string::npos);
    CHECK(count_sub(src, "bool do_is_equal") == 0);
    CHECK(count_sub(src, "class memory_resource") == 1);
    CHECK(count_sub(src, "class monotonic_buffer_resource {") == 1);

    const std::string sig = "bool is_equal(const memory_resource& __other) const noexcept";
    CHECK(count_sub(src, sig) == 1);
    const auto sig_at = src.find(sig);
    REQUIRE(sig_at != std::string::npos);
    const auto note_from = src.rfind("The same object compares equal.", sig_at);
    REQUIRE(note_from != std::string::npos);
    const auto note = src.substr(note_from, sig_at - note_from);
    CHECK(note.find("A different object compares unequal.") != std::string::npos);
    CHECK(note.find("Neither path allocates or frees.") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const auto eq = body_of(src, sig);
    REQUIRE(!eq.empty());
    const std::string same = "if (this == &__other) return true;";
    const std::string differ = "return false;";
    const auto same_at = eq.find(same);
    const auto differ_at = eq.find(differ);
    REQUIRE(same_at != std::string::npos);
    REQUIRE(differ_at != std::string::npos);
    CHECK(same_at < differ_at);
    CHECK(count_sub(eq, same) == 1);
    CHECK(count_sub(eq, "return true;") == 1);
    CHECK(count_sub(eq, differ) == 1);
    CHECK(eq.find("allocate") == std::string::npos);
    CHECK(eq.find("deallocate") == std::string::npos);
    CHECK(eq.find("free") == std::string::npos);
    CHECK(eq.find("malloc") == std::string::npos);
    CHECK(eq.find("new") == std::string::npos);
    CHECK(eq.find("delete") == std::string::npos);
    CHECK(eq.find("__prism_check") == std::string::npos);
    CHECK(eq.find("__builtin_trap") == std::string::npos);
    CHECK(eq.find("_M_") == std::string::npos);
    CHECK(eq.find("do_is_equal") == std::string::npos);

    const auto after_same = eq.substr(same_at + same.size());
    const auto after_differ = after_same.find(differ);
    REQUIRE(after_differ != std::string::npos);
    const auto between = after_same.substr(0, after_differ);
    for (unsigned char c : between) {
        CHECK((c == ' ' || c == '\t' || c == '\n' || c == '\r'));
    }
    const auto tail = after_same.substr(after_differ + differ.size());
    for (unsigned char c : tail) {
        CHECK((c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '}'));
    }

    // allocate and deallocate stay the functions already on monotonic_buffer_resource.
    CHECK(count_sub(src, "void* allocate(size_t __bytes, size_t __alignment)") == 1);
    CHECK(count_sub(src, "void deallocate(void* __p, size_t __bytes, size_t __alignment)") == 1);
    CHECK(count_sub(src, "__builtin_trap()") == 1);

    const auto alloc = body_of(src, "void* allocate(size_t __bytes, size_t __alignment)");
    REQUIRE(!alloc.empty());
    CHECK(alloc.find("is_equal") == std::string::npos);
    CHECK(alloc.find("__builtin_trap") == std::string::npos);
    CHECK(alloc.find("free(") == std::string::npos);
    const auto fits_ret = alloc.find("if (!__fits) return nullptr;");
    const auto formed = alloc.find("_M_buf +");
    REQUIRE(fits_ret != std::string::npos);
    REQUIRE(formed != std::string::npos);
    CHECK(fits_ret < formed);

    const auto de = body_of(src, "void deallocate(void* __p, size_t __bytes, size_t __alignment)");
    REQUIRE(!de.empty());
    CHECK(de.find("is_equal") == std::string::npos);
    const std::string zero = "if (__p == nullptr && __bytes == 0) return;";
    const std::string positive = "if (__p == nullptr && __bytes > 0)";
    const std::string check =
        "__prism_check(0, \"PTR-NULL-DEREF\", \"memory_resource::deallocate null pointer\")";
    const auto zero_at = de.find(zero);
    const auto positive_at = de.find(positive);
    const auto check_at = de.find(check);
    const auto trap_at = de.find("__builtin_trap()");
    REQUIRE(zero_at != std::string::npos);
    REQUIRE(positive_at != std::string::npos);
    REQUIRE(check_at != std::string::npos);
    REQUIRE(trap_at != std::string::npos);
    CHECK(zero_at < positive_at);
    CHECK(positive_at < check_at);
    CHECK(check_at < trap_at);
    CHECK(de.find("free(") == std::string::npos);
    CHECK(de.find("::operator delete") == std::string::npos);
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
    const auto after_trap = arm.substr(arm_trap + std::string("__builtin_trap();").size());
    for (unsigned char c : after_trap) {
        CHECK((c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '}'));
    }
}
