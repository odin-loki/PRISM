// The <iterator> model is partial. std::begin and std::end take T (&)[N]
// only. begin is a pointer to the first element. end is one past the last.
// N == 0 yields the same pointer from both, and neither function
// dereferences it. A pointer argument does not match begin or end.
// std::distance is the two-pointer overload only. A count that does not
// fit in ptrdiff_t is __prism_check then __builtin_trap, not a wrapped
// ptrdiff_t. This is not a proof that the two pointers address the same
// array.
//
// This case reads the header as text. It does not `#include` the model:
// doctest already pulls in the platform library, and defining
// _GLIBCXX_ITERATOR here would hide that header or duplicate std::begin.
// The trace is tests/cxx_models/iterator_trace.cpp. This is not a proof
// that an iterator stays in range.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "iterator"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Drop // comments. The header has no block comments and no string literals.
std::string strip_line_comments(std::string_view src) {
    std::string out;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            if (i < src.size()) out.push_back('\n');
            continue;
        }
        out.push_back(src[i]);
    }
    return out;
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
            if (src[i] == '{')
                ++depth;
            else if (src[i] == '}') {
                if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
            }
        }
        return {};
    }
}

std::size_t count_occ(std::string_view src, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = src.find(needle, at)) != std::string_view::npos; at += needle.size()) ++n;
    return n;
}

}  // namespace

TEST_CASE("cxx iterator model: array begin and end do not dereference and a pointer is not declared") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE(src.find("This is not a proof that an iterator stays in range.") != std::string::npos);
    REQUIRE(src.find("This is not a proof that the two pointers address the same array.") != std::string::npos);
    CHECK(src.find("#ifndef _GLIBCXX_ITERATOR") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_ITERATOR 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std {") != std::string::npos);
    CHECK(src.find("#include <cstddef>") != std::string::npos);
    CHECK(src.find("#include <cstdint>") != std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <string>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("#include <ranges>") == std::string::npos);
    CHECK(src.find("/*") == std::string::npos);
    CHECK(src.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(count_occ(src, "extern \"C\" void __prism_check(int, const char*, const char*);") == 1);
    CHECK(src.find("__prism_check(int, const char*, const char*) {") == std::string::npos);

    const std::string code = strip_line_comments(src);
    CHECK(count_occ(code, "#include <") == 2);
    CHECK(code.find("#include <cstddef>") != std::string::npos);
    CHECK(code.find("#include <cstdint>") != std::string::npos);
    const std::string begin_sig = "constexpr _Tp* begin(_Tp (&__arr)[_Nm]) noexcept";
    const std::string end_sig = "constexpr _Tp* end(_Tp (&__arr)[_Nm]) noexcept";
    const std::string zero_begin_sig = "constexpr auto begin(_Arr& __arr) noexcept";
    const std::string zero_end_sig = "constexpr auto end(_Arr& __arr) noexcept";
    const std::string distance_sig = "constexpr std::ptrdiff_t distance(_Tp* __first, _Tp* __last) noexcept";
    CHECK(count_occ(code, begin_sig) == 1);
    CHECK(count_occ(code, end_sig) == 1);
    CHECK(count_occ(code, zero_begin_sig) == 1);
    CHECK(count_occ(code, zero_end_sig) == 1);
    CHECK(count_occ(code, distance_sig) == 1);
    CHECK(count_occ(code, "begin(") == 2);
    CHECK(count_occ(code, "end(") == 2);
    CHECK(count_occ(code, "distance(") == 1);
    CHECK(count_occ(code, "requires(sizeof(_Arr) == 0)") == 2);
    // Two _Tp* return types, four _Tp* parameters, and the two const char*
    // of the check declaration. No other star: begin, end, and distance do
    // not dereference.
    CHECK(count_occ(code, "*") == 8);
    CHECK(code.find("reverse_iterator") == std::string::npos);
    CHECK(code.find("next") == std::string::npos);
    CHECK(code.find("prev") == std::string::npos);
    CHECK(code.find("advance") == std::string::npos);
    CHECK(code.find("input_iterator_tag") == std::string::npos);
    CHECK(code.find("random_access_iterator_tag") == std::string::npos);
    CHECK(code.find("_Container") == std::string::npos);
    CHECK(count_occ(code, "__prism_check(0, \"FUNC-CONTRACT\", \"std::distance subtraction does not fit in ptrdiff_t\")") ==
          1);

    const auto begin_body = body_of(code, begin_sig);
    const auto end_body = body_of(code, end_sig);
    const auto zero_begin_body = body_of(code, zero_begin_sig);
    const auto zero_end_body = body_of(code, zero_end_sig);
    REQUIRE(begin_body.find("return __arr;") != std::string::npos);
    REQUIRE(end_body.find("return __arr + _Nm;") != std::string::npos);
    REQUIRE(zero_begin_body.find("return __arr;") != std::string::npos);
    REQUIRE(zero_end_body.find("return __arr + 0;") != std::string::npos);
    CHECK(begin_body.find('*') == std::string::npos);
    CHECK(begin_body.find('[') == std::string::npos);
    CHECK(begin_body.find('+') == std::string::npos);
    CHECK(begin_body.find("__prism_check") == std::string::npos);
    CHECK(begin_body.find("distance") == std::string::npos);
    CHECK(end_body.find('*') == std::string::npos);
    CHECK(end_body.find('[') == std::string::npos);
    CHECK(end_body.find('&') == std::string::npos);
    CHECK(end_body.find("__prism_check") == std::string::npos);
    CHECK(end_body.find("distance") == std::string::npos);
    CHECK(zero_begin_body.find('*') == std::string::npos);
    CHECK(zero_begin_body.find('[') == std::string::npos);
    CHECK(zero_begin_body.find('+') == std::string::npos);
    CHECK(zero_begin_body.find("__prism_check") == std::string::npos);
    CHECK(zero_end_body.find('*') == std::string::npos);
    CHECK(zero_end_body.find('[') == std::string::npos);
    CHECK(zero_end_body.find("__prism_check") == std::string::npos);

    const auto trap = body_of(code, "void _Prism_distance_does_not_fit()");
    REQUIRE_FALSE(trap.empty());
    const auto check =
        trap.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::distance subtraction does not fit in ptrdiff_t\")");
    const auto builtin = trap.find("__builtin_trap()");
    REQUIRE(check != std::string::npos);
    REQUIRE(builtin != std::string::npos);
    CHECK(check < builtin);
    CHECK(trap.find("return") == std::string::npos);

    const auto diff = body_of(code, "std::ptrdiff_t _Prism_pointer_diff(_Tp* __first, _Tp* __last) noexcept");
    REQUIRE_FALSE(diff.empty());
    CHECK(diff.find("__last - __first") == std::string::npos);
    CHECK(diff.find('*') == std::string::npos);
    const auto neg_min = diff.find("return PTRDIFF_MIN;");
    const auto neg_fit = diff.find("return static_cast<std::ptrdiff_t>(0) - static_cast<std::ptrdiff_t>(__n);");
    const auto pos_fit = diff.find("return static_cast<std::ptrdiff_t>(__n);");
    const auto fail = diff.find("_Prism_distance_does_not_fit()");
    REQUIRE(neg_min != std::string::npos);
    REQUIRE(neg_fit != std::string::npos);
    REQUIRE(pos_fit != std::string::npos);
    REQUIRE(fail != std::string::npos);
    CHECK(fail < pos_fit);
    CHECK(diff.find("return -") == std::string::npos);

    const auto distance_body = body_of(code, distance_sig);
    REQUIRE_FALSE(distance_body.empty());
    CHECK(distance_body.find('*') == std::string::npos);
    CHECK(count_occ(distance_body, "return __last - __first;") == 1);
    CHECK(distance_body.find("if consteval") != std::string::npos);
    const auto folded = distance_body.find("return __last - __first;");
    const auto runtime = distance_body.find("return _Prism_pointer_diff(__first, __last);");
    REQUIRE(folded != std::string::npos);
    REQUIRE(runtime != std::string::npos);
    CHECK(distance_body.find("if consteval") < folded);
    CHECK(folded < runtime);
}
