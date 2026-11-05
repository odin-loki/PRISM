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
// std::size, std::data, and std::cdata cover an array (a bound and a
// zero-length overload) and, for size and data, one container lvalue.
// std::empty is the same two array shapes plus the container lvalue. The
// bound overload returns false and the zero-length overload returns true;
// neither loads an element or calls size, begin, or end. std::ssize is the
// same two array shapes plus the container lvalue, and a value past
// PTRDIFF_MAX is a contract check. std::iter_swap and std::iter_move are
// the pointer overloads. A null pointer is PTR-NULL-DEREF before a load.
//
// This case reads the header as text. It does not `#include` the model:
// doctest already pulls in the platform library, and defining
// _GLIBCXX_ITERATOR here would hide that header or duplicate std::begin.
// The trace is tests/cxx_models/iterator_trace.cpp. This is not a proof
// that an iterator stays in range.

#include <doctest/doctest.h>

#include "test_support.hpp"

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
    const std::string iter_swap_sig = "constexpr void iter_swap(_Tp* __a, _Tp* __b) noexcept";
    const std::string iter_move_sig = "constexpr _Tp iter_move(_Tp* __p) noexcept";
    const std::string size_sig = "constexpr size_t size(_Tp (&__arr)[_Nm]) noexcept";
    const std::string zero_size_sig = "constexpr size_t size(_Arr& __arr) noexcept";
    const std::string cont_size_sig = "constexpr auto size(const _Tp& __t) noexcept(noexcept(__t.size()))";
    const std::string empty_sig = "constexpr bool empty(_Tp (&)[_Nm]) noexcept";
    const std::string zero_empty_sig = "constexpr bool empty(_Arr&) noexcept";
    const std::string cont_empty_sig = "constexpr auto empty(const _Tp& __t) noexcept(noexcept(__t.empty()))";
    const std::string data_sig = "constexpr _Tp* data(_Tp (&__arr)[_Nm]) noexcept";
    const std::string zero_data_sig = "constexpr auto data(_Arr& __arr) noexcept";
    const std::string cont_data_sig = "constexpr auto data(const _Tp& __t) noexcept(noexcept(__t.data()))";
    const std::string cdata_sig = "constexpr const _Tp* cdata(_Tp (&__arr)[_Nm]) noexcept";
    const std::string zero_cdata_sig = "constexpr auto cdata(_Arr& __arr) noexcept";
    const std::string ssize_sig = "constexpr std::ptrdiff_t ssize(_Tp (&)[_Nm]) noexcept";
    const std::string zero_ssize_sig = "constexpr std::ptrdiff_t ssize(_Arr&) noexcept";
    const std::string cont_ssize_sig = "constexpr auto ssize(const _Tp& __t) noexcept(noexcept(__t.size()))";
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
    CHECK(count_occ(code, iter_swap_sig) == 1);
    CHECK(count_occ(code, iter_move_sig) == 1);
    CHECK(count_occ(code, size_sig) == 1);
    CHECK(count_occ(code, zero_size_sig) == 1);
    CHECK(count_occ(code, cont_size_sig) == 1);
    CHECK(count_occ(code, "-> decltype(__t.size())") == 1);
    CHECK(count_occ(code, empty_sig) == 1);
    CHECK(count_occ(code, zero_empty_sig) == 1);
    CHECK(count_occ(code, cont_empty_sig) == 1);
    CHECK(count_occ(code, "-> decltype(__t.empty())") == 1);
    CHECK(count_occ(code, data_sig) == 1);
    CHECK(count_occ(code, zero_data_sig) == 1);
    CHECK(count_occ(code, cont_data_sig) == 1);
    CHECK(count_occ(code, "-> decltype(__t.data())") == 1);
    CHECK(count_occ(code, cdata_sig) == 1);
    CHECK(count_occ(code, zero_cdata_sig) == 1);
    CHECK(count_occ(code, ssize_sig) == 1);
    CHECK(count_occ(code, zero_ssize_sig) == 1);
    CHECK(count_occ(code, cont_ssize_sig) == 1);
    CHECK(count_occ(code, "-> decltype(__t.size(), std::ptrdiff_t())") == 1);
    CHECK(count_occ(code, "begin(") == 2);
    CHECK(count_occ(code, "end(") == 2);
    CHECK(count_occ(code, "distance(") == 1);
    CHECK(count_occ(code, "next(") == 2);
    CHECK(count_occ(code, "prev(") == 2);
    CHECK(count_occ(code, "advance(") == 1);
    CHECK(count_occ(code, "iter_swap(") == 1);
    CHECK(count_occ(code, "iter_move(") == 1);
    // ssize( contains size(. Two array size overloads, three ssize
    // overloads, three __t.size() spellings on container ssize, and four
    // spellings on the container size overload.
    CHECK(count_occ(code, "size(") == 12);
    // Two array empty overloads, then on the container overload the
    // signature, noexcept operand, decltype operand, and one call.
    CHECK(count_occ(code, "empty(") == 6);
    // Two array data overloads, two cdata overloads (the substring data(),
    // and four spellings on the container data overload).
    CHECK(count_occ(code, "data(") == 8);
    CHECK(count_occ(code, "cdata(") == 2);
    CHECK(count_occ(code, "ssize(") == 3);
    CHECK(count_occ(code, "requires(sizeof(_Arr) == 0)") == 7);
    // begin and end return _Tp*. distance and _Prism_pointer_diff take four
    // _Tp* parameters. The two next overloads add two _Tp* returns and two
    // _Tp* parameters. The two prev overloads add two more of each.
    // advance takes one _Tp*&. iter_swap takes two _Tp* parameters and
    // dereferences four times. iter_move takes one _Tp* parameter and
    // dereferences once. data and cdata each add one pointer return on the
    // bound overload. The check declaration adds two const char*.
    CHECK(count_occ(code, "*") == 27);
    CHECK(code.find("reverse_iterator") == std::string::npos);
    CHECK(count_occ(code, "constexpr bool empty(") == 2);
    CHECK(code.find("size(_Tp*") == std::string::npos);
    CHECK(code.find("size(const _Tp*") == std::string::npos);
    CHECK(code.find("empty(_Tp*") == std::string::npos);
    CHECK(code.find("empty(const _Tp*") == std::string::npos);
    CHECK(code.find("data(_Tp*") == std::string::npos);
    CHECK(code.find("data(const _Tp*") == std::string::npos);
    CHECK(code.find("cdata(_Tp*") == std::string::npos);
    CHECK(code.find("ssize(_Tp*") == std::string::npos);
    CHECK(code.find("ssize(_Arr*") == std::string::npos);
    CHECK(code.find("ssize(const _Tp*") == std::string::npos);
    CHECK(count_occ(code, "__t.size()") == 6);
    CHECK(count_occ(code, "__t.empty()") == 3);
    CHECK(count_occ(code, "__t.data()") == 3);
    CHECK(count_occ(src, "This is not PROVED and not CLEAN.") == 15);
    CHECK(count_occ(src, "a backward step") == 2);
    CHECK(code.find("input_iterator_tag") == std::string::npos);
    CHECK(code.find("random_access_iterator_tag") == std::string::npos);
    CHECK(code.find("_Container") == std::string::npos);
    CHECK(count_occ(code, "__prism_check(0, \"FUNC-CONTRACT\", \"std::distance subtraction does not fit in ptrdiff_t\")") ==
          1);
    CHECK(count_occ(code, "__prism_check(0, \"FUNC-CONTRACT\", \"std::next offset is negative\")") == 1);
    CHECK(count_occ(code, "__prism_check(0, \"FUNC-CONTRACT\", \"std::prev offset is negative\")") == 1);
    CHECK(count_occ(code, "__prism_check(0, \"FUNC-CONTRACT\", \"std::ssize array bound does not fit in ptrdiff_t\")") ==
          1);
    CHECK(count_occ(code, "__prism_check(0, \"FUNC-CONTRACT\", \"std::ssize value does not fit in ptrdiff_t\")") == 1);
    CHECK(count_occ(code, "__prism_check(0, \"PTR-NULL-DEREF\", \"std::iter_swap null pointer\")") == 1);
    CHECK(count_occ(code, "__prism_check(0, \"PTR-NULL-DEREF\", \"std::iter_move null pointer\")") == 1);

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

    const auto size_body = body_of(code, size_sig);
    const auto zero_size_body = body_of(code, zero_size_sig);
    REQUIRE(size_body.find("return _Nm;") != std::string::npos);
    REQUIRE(zero_size_body.find("return 0;") != std::string::npos);
    CHECK(count_occ(size_body, "*") == 0);
    CHECK(count_occ(zero_size_body, "*") == 0);
    CHECK(size_body.find("__arr") == std::string::npos);
    CHECK(size_body.find("begin") == std::string::npos);
    CHECK(size_body.find("__prism_check") == std::string::npos);
    CHECK(zero_size_body.find("__arr") == std::string::npos);
    CHECK(zero_size_body.find("__prism_check") == std::string::npos);

    const auto cont_size_body = body_of(code, cont_size_sig);
    REQUIRE(cont_size_body.find("return __t.size();") != std::string::npos);
    CHECK(count_occ(cont_size_body, "return __t.size();") == 1);
    CHECK(count_occ(cont_size_body, "*") == 0);
    CHECK(cont_size_body.find("return 0") == std::string::npos);
    CHECK(cont_size_body.find("__prism_check") == std::string::npos);

    const auto empty_body = body_of(code, empty_sig);
    const auto zero_empty_body = body_of(code, zero_empty_sig);
    REQUIRE(empty_body.find("return false;") != std::string::npos);
    REQUIRE(zero_empty_body.find("return true;") != std::string::npos);
    CHECK(count_occ(empty_body, "return false;") == 1);
    CHECK(count_occ(zero_empty_body, "return true;") == 1);
    for (const auto* b : {&empty_body, &zero_empty_body}) {
        CHECK(count_occ(*b, "*") == 0);
        CHECK(b->find('[') == std::string::npos);
        CHECK(b->find('+') == std::string::npos);
        CHECK(b->find("__arr") == std::string::npos);
        CHECK(b->find("__t") == std::string::npos);
        CHECK(b->find("begin") == std::string::npos);
        CHECK(b->find("end") == std::string::npos);
        CHECK(b->find("size") == std::string::npos);
        CHECK(b->find("data") == std::string::npos);
        CHECK(b->find("__prism_check") == std::string::npos);
        CHECK(b->find("__builtin_trap") == std::string::npos);
    }

    const auto cont_empty_body = body_of(code, cont_empty_sig);
    REQUIRE(cont_empty_body.find("return __t.empty();") != std::string::npos);
    CHECK(count_occ(cont_empty_body, "return __t.empty();") == 1);
    CHECK(count_occ(cont_empty_body, "size(") == 0);
    CHECK(cont_empty_body.find("return false") == std::string::npos);
    CHECK(cont_empty_body.find("return true") == std::string::npos);
    CHECK(count_occ(cont_empty_body, "*") == 0);
    CHECK(cont_empty_body.find("__prism_check") == std::string::npos);

    const auto data_body = body_of(code, data_sig);
    const auto zero_data_body = body_of(code, zero_data_sig);
    REQUIRE(data_body.find("return __arr;") != std::string::npos);
    REQUIRE(zero_data_body.find("return __arr;") != std::string::npos);
    CHECK(count_occ(data_body, "*") == 0);
    CHECK(count_occ(zero_data_body, "*") == 0);
    CHECK(data_body.find('+') == std::string::npos);
    CHECK(data_body.find("size") == std::string::npos);
    CHECK(data_body.find("empty") == std::string::npos);
    CHECK(zero_data_body.find('+') == std::string::npos);
    CHECK(data_body.find("__prism_check") == std::string::npos);
    CHECK(zero_data_body.find("__prism_check") == std::string::npos);

    const auto cont_data_body = body_of(code, cont_data_sig);
    REQUIRE(cont_data_body.find("return __t.data();") != std::string::npos);
    CHECK(count_occ(cont_data_body, "return __t.data();") == 1);
    CHECK(count_occ(cont_data_body, "*") == 0);
    CHECK(cont_data_body.find("nullptr") == std::string::npos);
    CHECK(cont_data_body.find("cdata(") == std::string::npos);
    CHECK(cont_data_body.find("__prism_check") == std::string::npos);

    const auto cdata_body = body_of(code, cdata_sig);
    const auto zero_cdata_body = body_of(code, zero_cdata_sig);
    REQUIRE(cdata_body.find("return __arr;") != std::string::npos);
    REQUIRE(zero_cdata_body.find("return __arr;") != std::string::npos);
    CHECK(count_occ(cdata_body, "*") == 0);
    CHECK(count_occ(zero_cdata_body, "*") == 0);
    CHECK(cdata_body.find("data") == std::string::npos);
    CHECK(zero_cdata_body.find("data") == std::string::npos);
    CHECK(cdata_body.find("__prism_check") == std::string::npos);
    CHECK(zero_cdata_body.find("__prism_check") == std::string::npos);

    const auto ssize_body = body_of(code, ssize_sig);
    const auto zero_ssize_body = body_of(code, zero_ssize_sig);
    REQUIRE_FALSE(ssize_body.empty());
    REQUIRE(zero_ssize_body.find("return 0;") != std::string::npos);
    const auto ssize_check = ssize_body.find("_Prism_ssize_array_bound_does_not_fit()");
    const auto ssize_ret = ssize_body.find("return static_cast<std::ptrdiff_t>(_Nm);");
    REQUIRE(ssize_check != std::string::npos);
    REQUIRE(ssize_ret != std::string::npos);
    CHECK(ssize_check < ssize_ret);
    CHECK(ssize_body.find("static_cast<std::ptrdiff_t>(_Nm)") != std::string::npos);
    CHECK(count_occ(ssize_body, "*") == 0);
    CHECK(count_occ(zero_ssize_body, "*") == 0);
    CHECK(zero_ssize_body.find("__prism_check") == std::string::npos);

    const auto cont_ssize_body = body_of(code, cont_ssize_sig);
    REQUIRE(cont_ssize_body.find("const auto __n = __t.size();") != std::string::npos);
    const auto cont_ssize_check = cont_ssize_body.find("_Prism_ssize_value_does_not_fit()");
    const auto cont_ssize_ret = cont_ssize_body.find("return static_cast<std::ptrdiff_t>(__n);");
    REQUIRE(cont_ssize_check != std::string::npos);
    REQUIRE(cont_ssize_ret != std::string::npos);
    CHECK(prism_test::first_before(cont_ssize_body, "__n > PTRDIFF_MAX", cont_ssize_check));
    CHECK(cont_ssize_check < cont_ssize_ret);
    CHECK(count_occ(cont_ssize_body, "*") == 0);
    CHECK(count_occ(cont_ssize_body, "__t.size()") == 1);

    const auto swap_body = body_of(code, iter_swap_sig);
    REQUIRE_FALSE(swap_body.empty());
    const auto swap_null = swap_body.find("_Prism_iter_swap_null()");
    const auto swap_self = swap_body.find("return;");
    const auto swap_load = swap_body.find("_Tp __tmp = *__a;");
    REQUIRE(swap_null != std::string::npos);
    REQUIRE(swap_self != std::string::npos);
    REQUIRE(swap_load != std::string::npos);
    CHECK(prism_test::first_before(swap_body, "__a == nullptr || __b == nullptr", swap_null));
    CHECK(swap_null < swap_self);
    CHECK(swap_self < swap_load);
    CHECK(swap_body.find("*__a = *__b;") != std::string::npos);
    CHECK(swap_body.find("*__b = __tmp;") != std::string::npos);
    CHECK(count_occ(swap_body, "*") == 4);
    CHECK(swap_body.substr(0, swap_load).find('*') == std::string::npos);

    const auto move_body = body_of(code, iter_move_sig);
    REQUIRE_FALSE(move_body.empty());
    const auto move_null = move_body.find("_Prism_iter_move_null()");
    const auto move_load = move_body.find("return *__p;");
    REQUIRE(move_null != std::string::npos);
    REQUIRE(move_load != std::string::npos);
    CHECK(prism_test::first_before(move_body, "__p == nullptr", move_null));
    CHECK(move_null < move_load);
    CHECK(count_occ(move_body, "*") == 1);
    CHECK(move_body.find("T{}") == std::string::npos);
    CHECK(move_body.substr(0, move_load).find('*') == std::string::npos);

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
    CHECK(prism_test::first_before(distance_body, "if consteval", folded));
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
    CHECK(prism_test::first_before(next_n_body, "__n < 0", call_neg));
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
    CHECK(prism_test::first_before(prev_n_body, "__n < 0", prev_call_neg));
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
