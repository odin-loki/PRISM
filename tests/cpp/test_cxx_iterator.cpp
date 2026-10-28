// The <iterator> model is partial. std::begin and std::end take T (&)[N]
// only. begin is a pointer to the first element. end is one past the last.
// N == 0 yields the same pointer from both, and neither function
// dereferences it. A pointer argument does not match begin or end.
// std::distance is the two-pointer overload only. A count that does not
// fit in ptrdiff_t is __prism_check then __builtin_trap, not a wrapped
// ptrdiff_t. This is not a proof that the two pointers address the same
// array.
//
// std::next is the pointer overloads only. next(p) returns p + 1.
// next(p, 0) returns p and does not add. next(p, n) for n > 0 returns
// p + n. n < 0 is __prism_check then __builtin_trap and does not form
// p + n. This is not a proof that the pointer addresses an array.
//
// std::prev is the pointer overloads only. prev(p) returns p - 1.
// prev(p, 0) returns p and does not subtract. prev(p, n) for n > 0
// returns p - n. n < 0 is __prism_check then __builtin_trap and does not
// form p - n. This is not a proof that the pointer addresses an array.
//
// std::advance is the pointer overload only. advance(p, 0) leaves p
// unchanged and does not add or subtract. advance(p, n) for n > 0 sets p
// to p + n. advance(p, n) for n < 0 sets p to p + n, a backward step, and
// does not trap. It does not call next or prev. reverse_iterator stays
// omitted. This is not PROVED and not CLEAN. This is not a proof that the
// pointer addresses an array.
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
    REQUIRE(src.find("This is not a proof that the pointer addresses an array.") != std::string::npos);
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
    CHECK(src.find("#include <cmath>") == std::string::npos);
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
    const std::string next_sig = "constexpr _Tp* next(_Tp* __p) noexcept";
    const std::string next_n_sig = "constexpr _Tp* next(_Tp* __p, std::ptrdiff_t __n) noexcept";
    const std::string prev_sig = "constexpr _Tp* prev(_Tp* __p) noexcept";
    const std::string prev_n_sig = "constexpr _Tp* prev(_Tp* __p, std::ptrdiff_t __n) noexcept";
    const std::string advance_sig = "constexpr void advance(_Tp*& __p, std::ptrdiff_t __n) noexcept";
    CHECK(count_occ(code, begin_sig) == 1);
    CHECK(count_occ(code, end_sig) == 1);
    CHECK(count_occ(code, zero_begin_sig) == 1);
    CHECK(count_occ(code, zero_end_sig) == 1);
    CHECK(count_occ(code, distance_sig) == 1);
    CHECK(count_occ(code, next_sig) == 1);
    CHECK(count_occ(code, next_n_sig) == 1);
    CHECK(count_occ(code, prev_sig) == 1);
    CHECK(count_occ(code, prev_n_sig) == 1);
    CHECK(count_occ(code, advance_sig) == 1);
    CHECK(count_occ(code, "begin(") == 2);
    CHECK(count_occ(code, "end(") == 2);
    CHECK(count_occ(code, "distance(") == 1);
    CHECK(count_occ(code, "next(") == 2);
    CHECK(count_occ(code, "prev(") == 2);
    CHECK(count_occ(code, "advance(") == 1);
    CHECK(count_occ(code, "requires(sizeof(_Arr) == 0)") == 2);
    // begin and end return _Tp*. distance and _Prism_pointer_diff take four
    // _Tp* parameters. The two next overloads add two _Tp* returns and two
    // _Tp* parameters. The two prev overloads add two more of each.
    // advance takes one _Tp*&. The check declaration adds two const char*.
    // No other star: these functions do not dereference.
    CHECK(count_occ(code, "*") == 17);
    CHECK(code.find("reverse_iterator") == std::string::npos);
    CHECK(count_occ(src, "This is not PROVED and not CLEAN.") == 2);
    CHECK(count_occ(src, "a backward step") == 2);
    CHECK(code.find("input_iterator_tag") == std::string::npos);
    CHECK(code.find("random_access_iterator_tag") == std::string::npos);
    CHECK(code.find("_Container") == std::string::npos);
    CHECK(count_occ(code, "__prism_check(0, \"FUNC-CONTRACT\", \"std::distance subtraction does not fit in ptrdiff_t\")") ==
          1);
    CHECK(count_occ(code, "__prism_check(0, \"FUNC-CONTRACT\", \"std::next offset is negative\")") == 1);
    CHECK(count_occ(code, "__prism_check(0, \"FUNC-CONTRACT\", \"std::prev offset is negative\")") == 1);

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

    const auto next_neg = body_of(code, "void _Prism_next_negative()");
    REQUIRE_FALSE(next_neg.empty());
    const auto next_check = next_neg.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::next offset is negative\")");
    const auto next_trap = next_neg.find("__builtin_trap()");
    REQUIRE(next_check != std::string::npos);
    REQUIRE(next_trap != std::string::npos);
    CHECK(next_check < next_trap);
    CHECK(next_neg.find("return") == std::string::npos);
    CHECK(next_neg.find('+') == std::string::npos);
    CHECK(next_neg.find("__p +") == std::string::npos);

    const auto next_body = body_of(code, next_sig);
    REQUIRE(next_body.find("return __p + 1;") != std::string::npos);
    CHECK(count_occ(next_body, "+") == 1);
    CHECK(next_body.find("__prism_check") == std::string::npos);
    CHECK(next_body.find("__n") == std::string::npos);
    CHECK(next_body.find('*') == std::string::npos);

    const auto next_n_body = body_of(code, next_n_sig);
    REQUIRE_FALSE(next_n_body.empty());
    const auto call_neg = next_n_body.find("_Prism_next_negative()");
    const auto zero_ret = next_n_body.find("return __p;");
    const auto pos_ret = next_n_body.find("return __p + __n;");
    REQUIRE(call_neg != std::string::npos);
    REQUIRE(zero_ret != std::string::npos);
    REQUIRE(pos_ret != std::string::npos);
    CHECK(next_n_body.find("__n < 0") != std::string::npos);
    CHECK(next_n_body.find("__n < 0") < call_neg);
    CHECK(call_neg < zero_ret);
    CHECK(zero_ret < pos_ret);
    CHECK(count_occ(next_n_body, "+") == 1);
    CHECK(next_n_body.substr(0, pos_ret).find('+') == std::string::npos);
    CHECK(next_n_body.find("return __p + 0") == std::string::npos);
    CHECK(next_n_body.find("__prism_check") == std::string::npos);
    CHECK(next_n_body.find('*') == std::string::npos);

    const auto prev_neg = body_of(code, "void _Prism_prev_negative()");
    REQUIRE_FALSE(prev_neg.empty());
    const auto prev_check = prev_neg.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::prev offset is negative\")");
    const auto prev_trap = prev_neg.find("__builtin_trap()");
    REQUIRE(prev_check != std::string::npos);
    REQUIRE(prev_trap != std::string::npos);
    CHECK(prev_check < prev_trap);
    CHECK(prev_neg.find("return") == std::string::npos);
    CHECK(prev_neg.find("__p -") == std::string::npos);
    CHECK(prev_neg.find("__p-") == std::string::npos);
    // FUNC-CONTRACT is the only hyphen. The helper does not form p - n.
    CHECK(count_occ(prev_neg, "-") == 1);
    CHECK(prev_neg.find(" - ") == std::string::npos);

    const auto prev_body = body_of(code, prev_sig);
    REQUIRE(prev_body.find("return __p - 1;") != std::string::npos);
    CHECK(count_occ(prev_body, "-") == 1);
    CHECK(prev_body.find("__prism_check") == std::string::npos);
    CHECK(prev_body.find("__n") == std::string::npos);
    CHECK(prev_body.find('*') == std::string::npos);

    const auto prev_n_body = body_of(code, prev_n_sig);
    REQUIRE_FALSE(prev_n_body.empty());
    const auto prev_call_neg = prev_n_body.find("_Prism_prev_negative()");
    const auto prev_zero_ret = prev_n_body.find("return __p;");
    const auto prev_pos_ret = prev_n_body.find("return __p - __n;");
    REQUIRE(prev_call_neg != std::string::npos);
    REQUIRE(prev_zero_ret != std::string::npos);
    REQUIRE(prev_pos_ret != std::string::npos);
    CHECK(prev_n_body.find("__n < 0") != std::string::npos);
    CHECK(prev_n_body.find("__n < 0") < prev_call_neg);
    CHECK(prev_call_neg < prev_zero_ret);
    CHECK(prev_zero_ret < prev_pos_ret);
    CHECK(count_occ(prev_n_body, "-") == 1);
    CHECK(prev_n_body.substr(0, prev_pos_ret).find('-') == std::string::npos);
    CHECK(prev_n_body.find("return __p - 0") == std::string::npos);
    CHECK(prev_n_body.find("__prism_check") == std::string::npos);
    CHECK(prev_n_body.find('*') == std::string::npos);

    const auto advance_body = body_of(code, advance_sig);
    REQUIRE_FALSE(advance_body.empty());
    const auto adv_zero = advance_body.find("__n == 0");
    const auto adv_ret = advance_body.find("return;");
    const auto adv_pos = advance_body.find("__n > 0");
    const auto adv_neg = advance_body.find("__n < 0");
    const auto adv_add = advance_body.find("__p = __p + __n;");
    REQUIRE(adv_zero != std::string::npos);
    REQUIRE(adv_ret != std::string::npos);
    REQUIRE(adv_pos != std::string::npos);
    REQUIRE(adv_neg != std::string::npos);
    REQUIRE(adv_add != std::string::npos);
    const auto adv_add2 = advance_body.find("__p = __p + __n;", adv_add + 1);
    REQUIRE(adv_add2 != std::string::npos);
    CHECK(adv_zero < adv_ret);
    CHECK(adv_ret < adv_pos);
    CHECK(adv_pos < adv_add);
    CHECK(adv_add < adv_neg);
    CHECK(adv_neg < adv_add2);
    CHECK(count_occ(advance_body, "__p = __p + __n;") == 2);
    CHECK(count_occ(advance_body, "+") == 2);
    CHECK(count_occ(advance_body, "-") == 0);
    const auto zero_slice = advance_body.substr(adv_zero, adv_pos - adv_zero);
    CHECK(zero_slice.find("return;") != std::string::npos);
    CHECK(zero_slice.find('+') == std::string::npos);
    CHECK(zero_slice.find('-') == std::string::npos);
    const auto pos_slice = advance_body.substr(adv_pos, adv_neg - adv_pos);
    CHECK(count_occ(pos_slice, "__p = __p + __n;") == 1);
    CHECK(count_occ(pos_slice, "+") == 1);
    CHECK(pos_slice.find('-') == std::string::npos);
    const auto neg_slice = advance_body.substr(adv_neg);
    CHECK(count_occ(neg_slice, "__p = __p + __n;") == 1);
    CHECK(count_occ(neg_slice, "+") == 1);
    CHECK(neg_slice.find('-') == std::string::npos);
    CHECK(neg_slice.find("__prism_check") == std::string::npos);
    CHECK(neg_slice.find("__builtin_trap") == std::string::npos);
    CHECK(advance_body.find("next") == std::string::npos);
    CHECK(advance_body.find("prev") == std::string::npos);
    CHECK(advance_body.find("__prism_check") == std::string::npos);
    CHECK(advance_body.find("__builtin_trap") == std::string::npos);
    CHECK(advance_body.find('*') == std::string::npos);
    CHECK(advance_body.find("return __p") == std::string::npos);
}
