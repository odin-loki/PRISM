// The <bitset> model is one extensionless file. operator[] on an index >= N
// is MEM-OOB-READ, then __builtin_trap, before any word is read.
// bitset<0>::operator[](0) takes that path and does not read a bit. That
// path does not return a bit. test() still reports an out-of-range position.
// This is not PROVED and not CLEAN.
// docs/finish-notes/bitset-index-trap.md
// CMake's prism_tests source list is explicit (not a glob): this file is one
// line in _prism_test_sources. That line was added. No line was removed.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path header_path() {
    return fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "bitset";
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::size_t count_of(std::string_view s, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = s.find(needle, at)) != std::string_view::npos; at += needle.size()) ++n;
    return n;
}

std::string body_of(std::string_view src, std::string_view sig) {
    const auto at = src.find(sig);
    if (at == std::string_view::npos) return {};
    const auto brace = src.find('{', at);
    if (brace == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
        }
    }
    return {};
}

constexpr const char* kMsg = "std::bitset::operator[] index is outside the bitset";

void check_oob_arm(const std::string& body) {
    const auto gate = body.find("if (__position >= _Nb)");
    const auto check = body.find(
        "__prism_check(0, \"MEM-OOB-READ\", \"std::bitset::operator[] index is outside the bitset\")");
    const auto trap = body.find("__builtin_trap();");
    REQUIRE(gate != std::string::npos);
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    CHECK(gate < check);
    CHECK(check < trap);
    const auto arm = body.substr(gate, trap - gate);
    CHECK(arm.find("_M_w") == std::string::npos);
    CHECK(arm.find("_M_words") == std::string::npos);
    CHECK(arm.find("_M_unchecked") == std::string::npos);
    CHECK(arm.find("return") == std::string::npos);
    CHECK(arm.find("__glibcxx_assert") == std::string::npos);
    CHECK(body.find("return false") == std::string::npos);
    CHECK(body.find("return true") == std::string::npos);
    CHECK(body.find("return 0") == std::string::npos);
    CHECK(body.find("return {}") == std::string::npos);
    CHECK(body.find("__glibcxx_assert") == std::string::npos);
    CHECK(body.find("bit_cast") == std::string::npos);
}

}  // namespace

TEST_CASE("cxx bitset model: operator[] traps before a word is read") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(src.find("bit_cast") == std::string::npos);
    CHECK(count_of(src, kMsg) == 3);  // the header note and the two checks
    CHECK(count_of(src, "__prism_check(0, \"MEM-OOB-READ\", \"std::bitset::operator[] index is outside the bitset\")") ==
          2);

    const auto ref = body_of(src, "constexpr reference operator[](size_t __position) noexcept");
    const auto cst = body_of(src, "constexpr bool operator[](size_t __position) const noexcept");
    REQUIRE_FALSE(ref.empty());
    REQUIRE_FALSE(cst.empty());
    check_oob_arm(ref);
    check_oob_arm(cst);

    CHECK(ref.find("_M_w") == std::string::npos);
    CHECK(ref.find("_M_words") == std::string::npos);
    CHECK(ref.find("_M_unchecked") == std::string::npos);
    const auto ref_trap = ref.find("__builtin_trap();");
    const auto ref_ret = ref.find("return reference(*this, __position);");
    REQUIRE(ref_trap != std::string::npos);
    REQUIRE(ref_ret != std::string::npos);
    CHECK(ref_trap < ref_ret);
    CHECK(ref.find("if constexpr (_Nb == 0)") != std::string::npos);

    const auto cst_trap = cst.find("__builtin_trap();");
    const auto cst_word = cst.find("_M_words._M_w[__position / _S_bpw]");
    const auto cst_bit = cst.find("return ((__w >> (__position % _S_bpw)) & 1UL) != 0;");
    REQUIRE(cst_trap != std::string::npos);
    REQUIRE(cst_word != std::string::npos);
    REQUIRE(cst_bit != std::string::npos);
    CHECK(cst_trap < cst_word);
    CHECK(cst_word < cst_bit);
    CHECK(cst.substr(0, cst_trap).find("_M_w") == std::string::npos);
    CHECK(cst.substr(0, cst_trap).find("_M_words") == std::string::npos);
    CHECK(cst.find("if constexpr (_Nb == 0)") != std::string::npos);
    const auto cst_else = cst.find("} else {");
    REQUIRE(cst_else != std::string::npos);
    CHECK(cst_trap < cst_else);
    CHECK(cst_else < cst_word);

    const auto tested = body_of(src, "constexpr bool test(size_t __position) const");
    REQUIRE_FALSE(tested.empty());
    CHECK(tested.find("_M_check(__position, __N(\"bitset::test\"))") != std::string::npos);
    CHECK(tested.find("return _M_unchecked_test(__position);") != std::string::npos);
    CHECK(tested.find("__prism_check") == std::string::npos);
    CHECK(tested.find("__builtin_trap") == std::string::npos);
    CHECK(tested.find(kMsg) == std::string::npos);

    for (const char* sig : {"constexpr bitset& set() noexcept",
                            "constexpr bitset& set(size_t __position, bool __val = true)",
                            "constexpr bitset& reset() noexcept",
                            "constexpr bitset& reset(size_t __position)",
                            "constexpr bitset& flip() noexcept",
                            "constexpr bitset& flip(size_t __position)",
                            "constexpr size_t count() const noexcept",
                            "constexpr unsigned long to_ulong() const",
                            "constexpr unsigned long long to_ullong() const",
                            "constexpr bool all() const noexcept",
                            "constexpr bool any() const noexcept",
                            "constexpr bool none() const noexcept",
                            "constexpr bitset& operator&=(const bitset& __rhs) noexcept",
                            "constexpr bitset& operator|=(const bitset& __rhs) noexcept",
                            "constexpr bitset& operator^=(const bitset& __rhs) noexcept",
                            "constexpr bitset operator~() const noexcept",
                            "constexpr bool operator==(const bitset& __rhs) const noexcept"}) {
        const auto body = body_of(src, sig);
        REQUIRE_FALSE(body.empty());
        CHECK(body.find("__prism_check") == std::string::npos);
        CHECK(body.find(kMsg) == std::string::npos);
        CHECK(body.find("__builtin_trap") == std::string::npos);
    }

    CHECK(src.find("_M_check(__position, __N(\"bitset::set\"))") != std::string::npos);
    CHECK(src.find("_M_check(__position, __N(\"bitset::reset\"))") != std::string::npos);
    CHECK(src.find("_M_check(__position, __N(\"bitset::flip\"))") != std::string::npos);
}
