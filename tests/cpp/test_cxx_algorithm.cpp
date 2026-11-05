// Differential test of the partial <algorithm> model
// (src/prism/pir/models/cxx/algorithm). The header is a file, so
// `#include <algorithm>` is not EISDIR. CMake's prism_tests source list is
// explicit (not a glob).
//
// min and max return one of the two arguments. Equal arguments return the
// first. An inverted clamp is __prism_check then __builtin_trap, and that
// path does not return v. min of the extreme ints is not 0.
// min_element and max_element take two pointers. An inverted range is
// __prism_check then __builtin_trap and does not return a pointer. An empty
// range returns last and does not read an element. Equal elements keep the
// first. find takes two pointers and a const T&. An inverted range is
// __prism_check then __builtin_trap and does not read or invent a T. An
// empty range returns last and does not read. The result is the first
// operator== match, or last. find does not allocate. find_if takes two
// pointers and bool (*)(const T&). A null predicate is __prism_check then
// __builtin_trap and does not call. An inverted range is __prism_check
// then __builtin_trap and does not read or invent a T. An empty range
// returns last and does not read or call. count takes two pointers and a
// const T&. An inverted range is __prism_check then __builtin_trap and
// does not read. An empty range returns 0 and does not read. A tally that
// would not fit in ptrdiff_t is __prism_check then __builtin_trap and does
// not return a wrapped count. all_of, any_of, and none_of take two
// pointers and any callable. A null function-pointer predicate is
// __prism_check then __builtin_trap and does not call. An inverted range is __prism_check
// then __builtin_trap and does not read. An empty range does not read and
// does not call: all_of returns true, any_of returns false, none_of
// returns true. all_of returns false at the first element for which the
// predicate is false, else true. any_of returns true at the first element
// for which it is true, else false. none_of returns false at the first
// element for which it is true, else true. fill takes two pointers and a
// const T&. An inverted range is __prism_check then __builtin_trap and
// does not write. An empty range returns and does not write, even when
// the pointer is null. A null first on a non-empty range is __prism_check
// then __builtin_trap and does not write. Each element in [first, last)
// becomes the value. fill does not write at last. copy takes two pointers
// and a destination pointer. An inverted range is __prism_check then
// __builtin_trap and does not read or write. An empty range returns dest
// and does not read or write, even when dest is null. A null dest on a
// non-empty range is __prism_check then __builtin_trap and does not write.
// A dest in [first, last) is __prism_check then __builtin_trap and does
// not write. That scan compares pointers and does not read an element.
// Each element in [first, last) is copied. copy does not write at last.
// copy_n takes one pointer, a std::size_t count, and a destination pointer.
// n == 0 returns dest and does not read or write, even when dest or the
// source is null. A null source with n > 0 is __prism_check then
// __builtin_trap and does not read. A null dest with n > 0 is
// __prism_check then __builtin_trap and does not write. n elements are
// copied. copy_n does not write one past the last element written.
// copy_backward takes three pointers. An inverted range is __prism_check
// then __builtin_trap and does not read or write. An empty range returns
// dest and does not read or write, even when dest is null. That empty
// check is before the null-destination check. A null dest on a non-empty
// range is __prism_check then __builtin_trap and does not write. A dest
// in (first, last] is __prism_check then __builtin_trap and does not
// write. That scan compares pointers and does not read an element. The
// elements in [first, last) are copied from the back into the positions
// ending at dest. copy_backward does not write at dest and does not read
// the element at last. The result is the start of the written range.
// std::ranges::count_if takes T(&)[N] and bool (*)(const T&). A null
// predicate is __prism_check then __builtin_trap and does not call and
// does not load. The subscript stays below N. N == 0 returns 0 and does
// not load and does not call. Its trace is the ranges trace.
// This is not PROVED and not CLEAN. sort and for_each are not declared.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

namespace cxxh = prism_test::cxx;
using cxxh::fwd;
using cxxh::repo;
using cxxh::run_cmd;
using cxxh::run_lines;

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "algorithm"; }

struct AlgCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

AlgCtx& ctx() {
    static AlgCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <algorithm>\n#include <climits>\n"
                                  "int main() {\n"
                                  "  if (std::min(7, 3) != 3) return 1;\n"
                                  "  if (std::max(3, 7) != 7) return 2;\n"
                                  "  if (std::clamp(5, 3, 7) != 5) return 3;\n"
                                  "  if (std::min(INT_MIN, INT_MAX) == 0) return 4;\n"
                                  "  return 0;\n"
                                  "}\n",
                                  cxxh::asan_flags());
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("algorithm_trace.cpp"), "algorithm_trace", cxxh::asan_flags(), with_models);
}

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

bool syntax_only(const std::string& cxx, const fs::path& src, bool with_models, std::string* err) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    std::vector<std::string> argv{cxx, "-std=c++23", "-fsyntax-only", "-w"};
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back(src.string());
    return run_cmd(argv, nullptr, err);
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("cxx algorithm model: header is a file and returns an argument") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string text = slurp(header);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("_GLIBCXX_ALGORITHM") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("namespace std") != std::string::npos);
    CHECK(text.find("not a proof that a range is sorted") != std::string::npos);
    CHECK(text.find("fails to compile") != std::string::npos);
    CHECK(text.find("extern \"C\" void __prism_check(int ok, const char* cls, const char* msg);") != std::string::npos);
    CHECK(text.find("__prism_check(int ok, const char* cls, const char* msg) {") == std::string::npos);
    CHECK(text.find("not PROVED") != std::string::npos);
    CHECK(text.find("not CLEAN") != std::string::npos);
    for (const char* inc : {"<memory>", "<functional>", "<string>", "<iterator>", "<ranges>", "<vector>", "<compare>"})
        CHECK(text.find(inc) == std::string::npos);
    {
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            std::size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            // the one include is <bits/stl_pair.h>, for minmax/minmax_element
            if (line.compare(i, 8, "#include") == 0) CHECK(line.find("<bits/stl_pair.h>") != std::string::npos);
        }
    }
    for (const char* name : {"sort(", "for_each(", "fill_n"})
        CHECK(text.find(name) == std::string::npos);
    // count_if is std::ranges::count_if only. std::count_if is not declared.
    {
        const auto ranges_at = text.find("namespace ranges {");
        REQUIRE(ranges_at != std::string::npos);
        const auto code_at = text.find("#ifndef _GLIBCXX_ALGORITHM");
        REQUIRE(code_at != std::string::npos);
        CHECK(text.substr(code_at, ranges_at - code_at).find("count_if") == std::string::npos);
        CHECK(text.find("count_if(_Tp*") == std::string::npos);
        CHECK(text.find("count_if(_Arr*") == std::string::npos);
        CHECK(text.find("count_if(const _Tp*") == std::string::npos);
    }

    for (const char* name : {"all_of(", "any_of(", "none_of(", "find_if_not(", "copy_if(", "is_partitioned(",
                             "partition_point(", "minmax(", "minmax_element(", "is_heap_until(", "is_heap("})
        CHECK(text.find(name) != std::string::npos);

    const auto min_at = text.find("constexpr const _Tp& min(");
    const auto max_at = text.find("constexpr const _Tp& max(", min_at);
    const auto clamp_at = text.find("constexpr const _Tp& clamp(", max_at);
    REQUIRE(min_at != std::string::npos);
    REQUIRE(max_at != std::string::npos);
    REQUIRE(clamp_at != std::string::npos);
    const std::string min_body = text.substr(min_at, max_at - min_at);
    const std::string max_body = text.substr(max_at, clamp_at - max_at);
    const auto min_cond = min_body.find("!(__b < __a)");
    const auto min_a = min_body.find("return __a");
    const auto min_b = min_body.find("return __b");
    REQUIRE(min_cond != std::string::npos);
    REQUIRE(min_a != std::string::npos);
    REQUIRE(min_b != std::string::npos);
    CHECK(min_cond < min_a);
    CHECK(min_a < min_b);
    CHECK(min_body.find("+") == std::string::npos);
    const auto max_cond = max_body.find("!(__a < __b)");
    const auto max_a = max_body.find("return __a");
    const auto max_b = max_body.find("return __b");
    REQUIRE(max_cond != std::string::npos);
    REQUIRE(max_a != std::string::npos);
    REQUIRE(max_b != std::string::npos);
    CHECK(max_cond < max_a);
    CHECK(max_a < max_b);
    CHECK(max_body.find("+") == std::string::npos);

    const auto bad = text.find("if (__hi < __lo)", clamp_at);
    const auto lo_if = text.find("if (__v < __lo)", bad);
    REQUIRE(bad != std::string::npos);
    REQUIRE(lo_if != std::string::npos);
    const std::string bad_body = text.substr(bad, lo_if - bad);
    const auto check = bad_body.find("__prism_check");
    const auto trap = bad_body.find("__builtin_trap");
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    CHECK(check < trap);
    CHECK(bad_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(bad_body.find("clamp: hi < lo") != std::string::npos);
    CHECK(bad_body.find("return") == std::string::npos);
    const std::string rest = text.substr(lo_if);
    const auto ret_lo = rest.find("return __lo");
    const auto ret_hi = rest.find("return __hi");
    const auto ret_v = rest.find("return __v");
    REQUIRE(ret_lo != std::string::npos);
    REQUIRE(ret_hi != std::string::npos);
    REQUIRE(ret_v != std::string::npos);
    CHECK(ret_lo < ret_hi);
    CHECK(ret_hi < ret_v);

    const auto min_el_at = text.find("constexpr _Tp* min_element(_Tp* __first, _Tp* __last)");
    const auto max_el_at = text.find("constexpr _Tp* max_element(_Tp* __first, _Tp* __last)", min_el_at);
    const auto eq_at = text.find("concept _Prism_equality_comparable");
    const auto find_at = text.find("constexpr _Tp* find(_Tp* __first, _Tp* __last, const _Tp& __value)");
    REQUIRE(min_el_at != std::string::npos);
    REQUIRE(max_el_at != std::string::npos);
    REQUIRE(eq_at != std::string::npos);
    REQUIRE(find_at != std::string::npos);
    CHECK(min_el_at > clamp_at);
    CHECK(max_el_at < eq_at);
    CHECK(eq_at < find_at);
    CHECK(text.find("min_element(_Tp* __first, _Tp* __last,") == std::string::npos);
    CHECK(text.find("max_element(_Tp* __first, _Tp* __last,") == std::string::npos);
    CHECK(text.find("find(_Tp* __first, _Tp* __last, const _Tp& __value,") == std::string::npos);
    const std::string min_el_body = text.substr(min_el_at, max_el_at - min_el_at);
    const std::string max_el_body = text.substr(max_el_at, eq_at - max_el_at);
    const auto check_range = [](const std::string& body, const char* msg) {
        const auto bad = body.find("if (__last < __first)");
        const auto empty = body.find("if (__first == __last) return __last;");
        REQUIRE(bad != std::string::npos);
        REQUIRE(empty != std::string::npos);
        CHECK(bad < empty);
        const std::string bad_body = body.substr(bad, empty - bad);
        const auto check = bad_body.find("__prism_check");
        const auto trap = bad_body.find("__builtin_trap");
        REQUIRE(check != std::string::npos);
        REQUIRE(trap != std::string::npos);
        CHECK(check < trap);
        CHECK(bad_body.find("FUNC-CONTRACT") != std::string::npos);
        CHECK(bad_body.find(msg) != std::string::npos);
        CHECK(bad_body.find("return") == std::string::npos);
        CHECK(bad_body.find("*__") == std::string::npos);
        const std::string before_read = body.substr(0, empty);
        CHECK(before_read.find("*__") == std::string::npos);
        CHECK(body.find("_Tp{") == std::string::npos);
        CHECK(body.find("_Tp(") == std::string::npos);
        CHECK(body.find("_Tp __") == std::string::npos);
        CHECK(body.find("<=") == std::string::npos);
        const auto scan = body.find("while (__first != __last)", empty);
        REQUIRE(scan != std::string::npos);
        CHECK(body.find("*__", scan) != std::string::npos);
    };
    check_range(min_el_body, "min_element: last < first");
    check_range(max_el_body, "max_element: last < first");
    CHECK(min_el_body.find("if (*__first < *__result)") != std::string::npos);
    CHECK(max_el_body.find("if (*__result < *__first)") != std::string::npos);

    const auto find_if_at =
        text.find("constexpr _Tp* find_if(_Tp* __first, _Tp* __last, bool (*__pred)(const _Tp&))");
    const auto count_at =
        text.find("constexpr __PTRDIFF_TYPE__ count(_Tp* __first, _Tp* __last, const _Tp& __value)");
    REQUIRE(find_if_at != std::string::npos);
    REQUIRE(count_at != std::string::npos);
    CHECK(find_at < find_if_at);
    CHECK(find_if_at < count_at);
    CHECK(text.substr(0, count_at).find("return 0") == std::string::npos);
    const auto find_if_sig = text.find("find_if(", find_if_at);
    const auto count_sig = text.find("count(", count_at);
    REQUIRE(find_if_sig != std::string::npos);
    REQUIRE(count_sig != std::string::npos);
    CHECK(text.find("find_if(", find_if_sig + 8) == std::string::npos);
    CHECK(text.find("count(", count_sig + 6) == std::string::npos);

    const std::string find_body = text.substr(find_at, find_if_at - find_at);
    const auto find_sig = find_body.find("find(");
    REQUIRE(find_sig != std::string::npos);
    CHECK(find_body.find("find(", find_sig + 1) == std::string::npos);
    const auto find_bad = find_body.find("if (__last < __first)");
    const auto find_empty = find_body.find("if (__first == __last) return __last;");
    REQUIRE(find_bad != std::string::npos);
    REQUIRE(find_empty != std::string::npos);
    CHECK(find_bad < find_empty);
    const std::string find_bad_body = find_body.substr(find_bad, find_empty - find_bad);
    const auto find_check = find_bad_body.find("__prism_check");
    const auto find_trap = find_bad_body.find("__builtin_trap");
    REQUIRE(find_check != std::string::npos);
    REQUIRE(find_trap != std::string::npos);
    CHECK(find_check < find_trap);
    CHECK(find_bad_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(find_bad_body.find("find: last < first") != std::string::npos);
    CHECK(find_bad_body.find("return") == std::string::npos);
    CHECK(find_bad_body.find("*__") == std::string::npos);
    CHECK(find_bad_body.find("__value") == std::string::npos);
    CHECK(find_bad_body.find("_Tp{") == std::string::npos);
    CHECK(find_bad_body.find("_Tp(") == std::string::npos);
    CHECK(find_bad_body.find("_Tp __") == std::string::npos);
    CHECK(find_body.substr(0, find_empty).find("*__") == std::string::npos);
    const auto find_scan = find_body.find("while (__first != __last)", find_empty);
    REQUIRE(find_scan != std::string::npos);
    const std::string find_between = find_body.substr(find_empty, find_scan - find_empty);
    CHECK(find_between.find("*__") == std::string::npos);
    CHECK(find_between.find("__value") == std::string::npos);
    CHECK(find_body.find("_Tp{") == std::string::npos);
    CHECK(find_body.find("_Tp(") == std::string::npos);
    CHECK(find_body.find("_Tp __") == std::string::npos);
    CHECK(find_body.find("new") == std::string::npos);
    CHECK(find_body.find("malloc") == std::string::npos);
    CHECK(find_body.find("calloc") == std::string::npos);
    CHECK(find_body.find("realloc") == std::string::npos);
    const auto find_eq = find_body.find("if (*__first == __value)", find_scan);
    REQUIRE(find_eq != std::string::npos);
    const auto find_hit = find_body.find("return __first", find_eq);
    REQUIRE(find_hit != std::string::npos);
    const auto find_bump = find_body.find("++__first", find_hit);
    REQUIRE(find_bump != std::string::npos);
    const auto find_end = find_body.find("return __last", find_bump);
    REQUIRE(find_end != std::string::npos);
    CHECK(find_scan < find_eq);
    CHECK(find_eq < find_hit);
    CHECK(find_hit < find_bump);
    CHECK(find_bump < find_end);

    const std::string find_if_body = text.substr(find_if_at, count_at - find_if_at);
    const auto fi_brace = find_if_body.find('{');
    REQUIRE(fi_brace != std::string::npos);
    const std::string find_if_fn = find_if_body.substr(fi_brace);
    const auto fi_null = find_if_fn.find("if (__pred == nullptr)");
    const auto fi_bad = find_if_fn.find("if (__last < __first)");
    const auto fi_empty = find_if_fn.find("if (__first == __last) return __last;");
    REQUIRE(fi_null != std::string::npos);
    REQUIRE(fi_bad != std::string::npos);
    REQUIRE(fi_empty != std::string::npos);
    CHECK(fi_null < fi_bad);
    CHECK(fi_bad < fi_empty);
    const std::string fi_null_body = find_if_fn.substr(fi_null, fi_bad - fi_null);
    const auto fi_null_check = fi_null_body.find("__prism_check");
    const auto fi_null_trap = fi_null_body.find("__builtin_trap");
    REQUIRE(fi_null_check != std::string::npos);
    REQUIRE(fi_null_trap != std::string::npos);
    CHECK(fi_null_check < fi_null_trap);
    CHECK(fi_null_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(fi_null_body.find("find_if: null predicate") != std::string::npos);
    CHECK(fi_null_body.find("return") == std::string::npos);
    CHECK(fi_null_body.find("*__") == std::string::npos);
    CHECK(fi_null_body.find("__pred(") == std::string::npos);
    const std::string fi_bad_body = find_if_fn.substr(fi_bad, fi_empty - fi_bad);
    const auto fi_bad_check = fi_bad_body.find("__prism_check");
    const auto fi_bad_trap = fi_bad_body.find("__builtin_trap");
    REQUIRE(fi_bad_check != std::string::npos);
    REQUIRE(fi_bad_trap != std::string::npos);
    CHECK(fi_bad_check < fi_bad_trap);
    CHECK(fi_bad_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(fi_bad_body.find("find_if: last < first") != std::string::npos);
    CHECK(fi_bad_body.find("return") == std::string::npos);
    CHECK(fi_bad_body.find("*__") == std::string::npos);
    CHECK(fi_bad_body.find("__pred(") == std::string::npos);
    CHECK(fi_bad_body.find("_Tp{") == std::string::npos);
    CHECK(fi_bad_body.find("_Tp(") == std::string::npos);
    CHECK(fi_bad_body.find("_Tp __") == std::string::npos);
    CHECK(find_if_fn.substr(0, fi_empty).find("*__") == std::string::npos);
    CHECK(find_if_fn.substr(0, fi_empty).find("__pred(") == std::string::npos);
    const auto fi_scan = find_if_fn.find("while (__first != __last)", fi_empty);
    REQUIRE(fi_scan != std::string::npos);
    const std::string fi_between = find_if_fn.substr(fi_empty, fi_scan - fi_empty);
    CHECK(fi_between.find("*__") == std::string::npos);
    CHECK(fi_between.find("__pred(") == std::string::npos);
    const auto fi_call = find_if_fn.find("__pred(*__first)", fi_scan);
    REQUIRE(fi_call != std::string::npos);
    const auto fi_hit = find_if_fn.find("return __first", fi_call);
    REQUIRE(fi_hit != std::string::npos);
    const auto fi_bump = find_if_fn.find("++__first", fi_hit);
    REQUIRE(fi_bump != std::string::npos);
    const auto fi_end = find_if_fn.find("return __last", fi_bump);
    REQUIRE(fi_end != std::string::npos);
    CHECK(fi_scan < fi_call);
    CHECK(fi_call < fi_hit);
    CHECK(fi_hit < fi_bump);
    CHECK(fi_bump < fi_end);
    CHECK(find_if_fn.find("find_if(", fi_call) == std::string::npos);
    CHECK(find_if_body.find("_Tp{") == std::string::npos);
    CHECK(find_if_body.find("_Tp(") == std::string::npos);
    CHECK(find_if_body.find("_Tp __") == std::string::npos);
    CHECK(find_if_body.find("new") == std::string::npos);
    CHECK(find_if_body.find("malloc") == std::string::npos);
    CHECK(find_if_body.find("return 0") == std::string::npos);

    const auto all_of_at =
        text.find("constexpr bool all_of(_Tp* __first, _Tp* __last, _Pred __pred)");
    const auto any_of_at = text.find(
        "constexpr bool any_of(_Tp* __first, _Tp* __last, _Pred __pred)", all_of_at);
    const auto none_of_at = text.find(
        "constexpr bool none_of(_Tp* __first, _Tp* __last, _Pred __pred)", any_of_at);
    REQUIRE(all_of_at != std::string::npos);
    REQUIRE(any_of_at != std::string::npos);
    REQUIRE(none_of_at != std::string::npos);
    CHECK(count_at < all_of_at);
    CHECK(all_of_at < any_of_at);
    CHECK(any_of_at < none_of_at);
    const auto all_sig = text.find("all_of(", all_of_at);
    const auto any_sig = text.find("any_of(", any_of_at);
    const auto none_sig = text.find("none_of(", none_of_at);
    const auto fill_at =
        text.find("constexpr void fill(_Tp* __first, _Tp* __last, const _Tp& __value)");
    REQUIRE(all_sig != std::string::npos);
    REQUIRE(any_sig != std::string::npos);
    REQUIRE(none_sig != std::string::npos);
    REQUIRE(fill_at != std::string::npos);
    CHECK(text.find("all_of(", all_sig + 7) == std::string::npos);
    CHECK(text.find("any_of(", any_sig + 7) == std::string::npos);
    CHECK(text.find("none_of(", none_sig + 8) == std::string::npos);
    CHECK(none_of_at < fill_at);
    const auto fill_sig = text.find("fill(", fill_at);
    REQUIRE(fill_sig != std::string::npos);
    CHECK(text.find("fill(", fill_sig + 5) == std::string::npos);
    const std::string count_body = text.substr(count_at, all_of_at - count_at);
    const std::string all_body = text.substr(all_of_at, any_of_at - all_of_at);
    const std::string any_body = text.substr(any_of_at, none_of_at - any_of_at);
    const auto after_none_at = text.find("constexpr _Tp* find_if_not(", none_of_at);
    REQUIRE(after_none_at != std::string::npos);
    const std::string none_body = text.substr(none_of_at, after_none_at - none_of_at);
    const auto count_brace = count_body.find('{');
    REQUIRE(count_brace != std::string::npos);
    const std::string count_fn = count_body.substr(count_brace);
    const auto count_bad = count_fn.find("if (__last < __first)");
    const auto count_empty = count_fn.find("if (__first == __last) return 0;");
    REQUIRE(count_bad != std::string::npos);
    REQUIRE(count_empty != std::string::npos);
    CHECK(count_bad < count_empty);
    const std::string count_bad_body = count_fn.substr(count_bad, count_empty - count_bad);
    const auto count_check = count_bad_body.find("__prism_check");
    const auto count_trap = count_bad_body.find("__builtin_trap");
    REQUIRE(count_check != std::string::npos);
    REQUIRE(count_trap != std::string::npos);
    CHECK(count_check < count_trap);
    CHECK(count_bad_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(count_bad_body.find("count: last < first") != std::string::npos);
    CHECK(count_bad_body.find("return") == std::string::npos);
    CHECK(count_bad_body.find("*__") == std::string::npos);
    CHECK(count_bad_body.find("__value") == std::string::npos);
    CHECK(count_bad_body.find("_Tp{") == std::string::npos);
    CHECK(count_bad_body.find("_Tp(") == std::string::npos);
    CHECK(count_bad_body.find("_Tp __") == std::string::npos);
    CHECK(count_fn.substr(0, count_empty).find("*__") == std::string::npos);
    CHECK(count_fn.substr(0, count_empty).find("__value") == std::string::npos);
    const auto count_zero = count_fn.find("__n = 0", count_empty);
    const auto count_scan = count_fn.find("while (__first != __last)", count_empty);
    REQUIRE(count_zero != std::string::npos);
    REQUIRE(count_scan != std::string::npos);
    CHECK(count_empty < count_zero);
    CHECK(count_zero < count_scan);
    const std::string count_between = count_fn.substr(count_empty, count_scan - count_empty);
    CHECK(count_between.find("*__") == std::string::npos);
    CHECK(count_between.find("__value") == std::string::npos);
    const auto count_eq = count_fn.find("if (*__first == __value)", count_scan);
    REQUIRE(count_eq != std::string::npos);
    const auto count_fit =
        count_fn.find("if (__n == static_cast<__PTRDIFF_TYPE__>(__PTRDIFF_MAX__))", count_eq);
    REQUIRE(count_fit != std::string::npos);
    const auto count_inc = count_fn.find("++__n", count_fit);
    REQUIRE(count_inc != std::string::npos);
    const std::string count_fit_body = count_fn.substr(count_fit, count_inc - count_fit);
    const auto count_fit_check = count_fit_body.find("__prism_check");
    const auto count_fit_trap = count_fit_body.find("__builtin_trap");
    REQUIRE(count_fit_check != std::string::npos);
    REQUIRE(count_fit_trap != std::string::npos);
    CHECK(count_fit_check < count_fit_trap);
    CHECK(count_fit_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(count_fit_body.find("count: result does not fit in ptrdiff_t") != std::string::npos);
    CHECK(count_fit_body.find("return") == std::string::npos);
    CHECK(count_fit_body.find("*__") == std::string::npos);
    CHECK(count_fit_body.find("++__n") == std::string::npos);
    CHECK(count_fit_body.find("++__first") == std::string::npos);
    const auto count_bump = count_fn.find("++__first", count_inc);
    REQUIRE(count_bump != std::string::npos);
    const auto count_ret = count_fn.find("return __n", count_bump);
    REQUIRE(count_ret != std::string::npos);
    CHECK(count_scan < count_eq);
    CHECK(count_eq < count_fit);
    CHECK(count_fit < count_inc);
    CHECK(count_inc < count_bump);
    CHECK(count_bump < count_ret);
    CHECK(count_body.find("_Tp{") == std::string::npos);
    CHECK(count_body.find("_Tp(") == std::string::npos);
    CHECK(count_body.find("_Tp __") == std::string::npos);
    CHECK(count_body.find("new") == std::string::npos);
    CHECK(count_body.find("malloc") == std::string::npos);
    CHECK(count_body.find("calloc") == std::string::npos);
    CHECK(count_body.find("realloc") == std::string::npos);

    const auto check_quant = [](const std::string& body, const char* null_msg, const char* range_msg,
                                const char* empty_ret, const char* decide, const char* final_ret) {
        const auto brace = body.find('{');
        REQUIRE(brace != std::string::npos);
        const std::string fn = body.substr(brace);
        const auto null_if = fn.find("if (__pred == nullptr)");
        const auto bad = fn.find("if (__last < __first)");
        const std::string empty_stmt = std::string("if (__first == __last) return ") + empty_ret + ";";
        const auto empty = fn.find(empty_stmt);
        REQUIRE(null_if != std::string::npos);
        REQUIRE(bad != std::string::npos);
        REQUIRE(empty != std::string::npos);
        CHECK(null_if < bad);
        CHECK(bad < empty);
        const std::string null_body = fn.substr(null_if, bad - null_if);
        const auto null_check = null_body.find("__prism_check");
        const auto null_trap = null_body.find("__builtin_trap");
        REQUIRE(null_check != std::string::npos);
        REQUIRE(null_trap != std::string::npos);
        CHECK(null_check < null_trap);
        CHECK(null_body.find("FUNC-CONTRACT") != std::string::npos);
        CHECK(null_body.find(null_msg) != std::string::npos);
        CHECK(null_body.find("return") == std::string::npos);
        CHECK(null_body.find("*__") == std::string::npos);
        CHECK(null_body.find("__pred(") == std::string::npos);
        const std::string bad_body = fn.substr(bad, empty - bad);
        const auto bad_check = bad_body.find("__prism_check");
        const auto bad_trap = bad_body.find("__builtin_trap");
        REQUIRE(bad_check != std::string::npos);
        REQUIRE(bad_trap != std::string::npos);
        CHECK(bad_check < bad_trap);
        CHECK(bad_body.find("FUNC-CONTRACT") != std::string::npos);
        CHECK(bad_body.find(range_msg) != std::string::npos);
        CHECK(bad_body.find("return") == std::string::npos);
        CHECK(bad_body.find("*__") == std::string::npos);
        CHECK(bad_body.find("__pred(") == std::string::npos);
        CHECK(bad_body.find("_Tp{") == std::string::npos);
        CHECK(bad_body.find("_Tp(") == std::string::npos);
        CHECK(bad_body.find("_Tp __") == std::string::npos);
        CHECK(fn.substr(0, empty).find("*__") == std::string::npos);
        CHECK(fn.substr(0, empty).find("__pred(") == std::string::npos);
        const auto scan = fn.find("while (__first != __last)", empty);
        REQUIRE(scan != std::string::npos);
        const std::string between = fn.substr(empty, scan - empty);
        CHECK(between.find("*__") == std::string::npos);
        CHECK(between.find("__pred(") == std::string::npos);
        const auto decided = fn.find(decide, scan);
        REQUIRE(decided != std::string::npos);
        const auto bump = fn.find("++__first", decided);
        REQUIRE(bump != std::string::npos);
        const std::string final_stmt = std::string("return ") + final_ret;
        const auto end = fn.find(final_stmt, bump);
        REQUIRE(end != std::string::npos);
        CHECK(scan < decided);
        CHECK(decided < bump);
        CHECK(bump < end);
        CHECK(body.find("_Tp{") == std::string::npos);
        CHECK(body.find("_Tp(") == std::string::npos);
        CHECK(body.find("_Tp __") == std::string::npos);
        CHECK(body.find("new") == std::string::npos);
        CHECK(body.find("malloc") == std::string::npos);
        CHECK(body.find("return 0") == std::string::npos);
    };
    check_quant(all_body, "all_of: null predicate", "all_of: last < first", "true",
                "if (!__pred(*__first)) return false;", "true");
    check_quant(any_body, "any_of: null predicate", "any_of: last < first", "false",
                "if (__pred(*__first)) return true;", "false");
    check_quant(none_body, "none_of: null predicate", "none_of: last < first", "true",
                "if (__pred(*__first)) return false;", "true");
    CHECK(any_body.find("!__pred") == std::string::npos);
    CHECK(none_body.find("!__pred") == std::string::npos);

    const auto copy_at = text.find("constexpr _Tp* copy(_Tp* __first, _Tp* __last, _Tp* __dest)");
    REQUIRE(copy_at != std::string::npos);
    CHECK(fill_at < copy_at);
    const auto copy_sig = text.find("copy(", copy_at);
    REQUIRE(copy_sig != std::string::npos);
    CHECK(text.find("copy(", copy_sig + 5) == std::string::npos);
    const std::string fill_body = text.substr(fill_at, copy_at - fill_at);
    const auto fill_brace = fill_body.find('{');
    REQUIRE(fill_brace != std::string::npos);
    const std::string fill_fn = fill_body.substr(fill_brace);
    const auto fill_bad = fill_fn.find("if (__last < __first)");
    const auto fill_empty = fill_fn.find("if (__first == __last) return;");
    const auto fill_null = fill_fn.find("if (__first == nullptr)");
    const auto fill_scan = fill_fn.find("while (__first != __last)");
    REQUIRE(fill_bad != std::string::npos);
    REQUIRE(fill_empty != std::string::npos);
    REQUIRE(fill_null != std::string::npos);
    REQUIRE(fill_scan != std::string::npos);
    CHECK(fill_bad < fill_empty);
    CHECK(fill_empty < fill_null);
    CHECK(fill_null < fill_scan);
    const std::string fill_bad_body = fill_fn.substr(fill_bad, fill_empty - fill_bad);
    const auto fill_bad_check = fill_bad_body.find("__prism_check");
    const auto fill_bad_trap = fill_bad_body.find("__builtin_trap");
    REQUIRE(fill_bad_check != std::string::npos);
    REQUIRE(fill_bad_trap != std::string::npos);
    CHECK(fill_bad_check < fill_bad_trap);
    CHECK(fill_bad_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(fill_bad_body.find("std::fill last is before first") != std::string::npos);
    CHECK(fill_bad_body.find("return") == std::string::npos);
    CHECK(fill_bad_body.find("*__") == std::string::npos);
    CHECK(fill_bad_body.find("__value") == std::string::npos);
    const std::string fill_null_body = fill_fn.substr(fill_null, fill_scan - fill_null);
    const auto fill_null_check = fill_null_body.find("__prism_check");
    const auto fill_null_trap = fill_null_body.find("__builtin_trap");
    REQUIRE(fill_null_check != std::string::npos);
    REQUIRE(fill_null_trap != std::string::npos);
    CHECK(fill_null_check < fill_null_trap);
    CHECK(fill_null_body.find("PTR-NULL-DEREF") != std::string::npos);
    CHECK(fill_null_body.find("std::fill null destination") != std::string::npos);
    CHECK(fill_null_body.find("return") == std::string::npos);
    CHECK(fill_null_body.find("*__") == std::string::npos);
    CHECK(fill_null_body.find("__value") == std::string::npos);
    CHECK(fill_fn.substr(0, fill_scan).find("*__") == std::string::npos);
    CHECK(fill_fn.substr(0, fill_scan).find("__value") == std::string::npos);
    const auto fill_store = fill_fn.find("*__first = __value", fill_scan);
    REQUIRE(fill_store != std::string::npos);
    const auto fill_bump = fill_fn.find("++__first", fill_store);
    REQUIRE(fill_bump != std::string::npos);
    CHECK(fill_scan < fill_store);
    CHECK(fill_store < fill_bump);
    CHECK(fill_fn.find("*__first = __value", fill_bump) == std::string::npos);
    CHECK(fill_body.find("<=") == std::string::npos);
    CHECK(fill_body.find("_Tp{") == std::string::npos);
    CHECK(fill_body.find("_Tp(") == std::string::npos);
    CHECK(fill_body.find("_Tp __") == std::string::npos);
    CHECK(fill_body.find("new") == std::string::npos);
    CHECK(fill_body.find("malloc") == std::string::npos);
    CHECK(fill_body.find("return 0") == std::string::npos);

    const auto copy_n_at = text.find("constexpr _Tp* copy_n(_Tp* __first, std::size_t __n, _Tp* __dest)");
    REQUIRE(copy_n_at != std::string::npos);
    CHECK(copy_at < copy_n_at);
    const std::string copy_body = text.substr(copy_at, copy_n_at - copy_at);
    const auto copy_brace = copy_body.find('{');
    REQUIRE(copy_brace != std::string::npos);
    const std::string copy_fn = copy_body.substr(copy_brace);
    const auto copy_bad = copy_fn.find("if (__last < __first)");
    const auto copy_empty = copy_fn.find("if (__first == __last) return __dest;");
    const auto copy_null = copy_fn.find("if (__dest == nullptr)");
    const auto copy_scan = copy_fn.find("while (__scan != __last)");
    const auto copy_hit = copy_fn.find("if (__scan == __dest)");
    const auto copy_loop = copy_fn.find("while (__first != __last)");
    REQUIRE(copy_bad != std::string::npos);
    REQUIRE(copy_empty != std::string::npos);
    REQUIRE(copy_null != std::string::npos);
    REQUIRE(copy_scan != std::string::npos);
    REQUIRE(copy_hit != std::string::npos);
    REQUIRE(copy_loop != std::string::npos);
    CHECK(copy_bad < copy_empty);
    CHECK(copy_empty < copy_null);
    CHECK(copy_null < copy_scan);
    CHECK(copy_scan < copy_hit);
    CHECK(copy_hit < copy_loop);
    const std::string copy_bad_body = copy_fn.substr(copy_bad, copy_empty - copy_bad);
    const auto copy_bad_check = copy_bad_body.find("__prism_check");
    const auto copy_bad_trap = copy_bad_body.find("__builtin_trap");
    REQUIRE(copy_bad_check != std::string::npos);
    REQUIRE(copy_bad_trap != std::string::npos);
    CHECK(copy_bad_check < copy_bad_trap);
    CHECK(copy_bad_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(copy_bad_body.find("std::copy last is before first") != std::string::npos);
    CHECK(copy_bad_body.find("return") == std::string::npos);
    CHECK(copy_bad_body.find("*__") == std::string::npos);
    CHECK(copy_bad_body.find("__dest") == std::string::npos);
    const std::string copy_null_body = copy_fn.substr(copy_null, copy_scan - copy_null);
    const auto copy_null_check = copy_null_body.find("__prism_check");
    const auto copy_null_trap = copy_null_body.find("__builtin_trap");
    REQUIRE(copy_null_check != std::string::npos);
    REQUIRE(copy_null_trap != std::string::npos);
    CHECK(copy_null_check < copy_null_trap);
    CHECK(copy_null_body.find("PTR-NULL-DEREF") != std::string::npos);
    CHECK(copy_null_body.find("std::copy null destination") != std::string::npos);
    CHECK(copy_null_body.find("return") == std::string::npos);
    CHECK(copy_null_body.find("*__") == std::string::npos);
    CHECK(copy_fn.substr(0, copy_loop).find("*__") == std::string::npos);
    const std::string copy_scan_body = copy_fn.substr(copy_scan, copy_loop - copy_scan);
    const auto copy_scan_check = copy_scan_body.find("__prism_check");
    const auto copy_scan_trap = copy_scan_body.find("__builtin_trap");
    REQUIRE(copy_scan_check != std::string::npos);
    REQUIRE(copy_scan_trap != std::string::npos);
    CHECK(copy_scan_check < copy_scan_trap);
    CHECK(copy_scan_body.find("*__") == std::string::npos);
    CHECK(copy_scan_body.find("<") == std::string::npos);
    CHECK(copy_scan_body.find("return") == std::string::npos);
    CHECK(copy_scan_body.find("__scan == __dest") != std::string::npos);
    CHECK(copy_scan_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(copy_scan_body.find("std::copy dest lies in [first, last)") != std::string::npos);
    const auto copy_store = copy_fn.find("*__out = *__first", copy_loop);
    REQUIRE(copy_store != std::string::npos);
    const auto copy_bump_first = copy_fn.find("++__first", copy_store);
    REQUIRE(copy_bump_first != std::string::npos);
    const auto copy_bump_out = copy_fn.find("++__out", copy_store);
    REQUIRE(copy_bump_out != std::string::npos);
    const auto copy_ret = copy_fn.find("return __out", copy_bump_out);
    REQUIRE(copy_ret != std::string::npos);
    CHECK(copy_loop < copy_store);
    CHECK(copy_store < copy_bump_first);
    CHECK(copy_bump_first < copy_bump_out);
    CHECK(copy_bump_out < copy_ret);
    CHECK(copy_fn.find("*__out = *__first", copy_store + 1) == std::string::npos);
    CHECK(copy_fn.find("*__last") == std::string::npos);
    const auto copy_end = copy_body.rfind("\n}");
    REQUIRE(copy_end != std::string::npos);
    const std::string copy_only = copy_body.substr(0, copy_end);
    CHECK(copy_only.find("<=") == std::string::npos);
    CHECK(copy_only.find("_Tp{") == std::string::npos);
    CHECK(copy_only.find("_Tp(") == std::string::npos);
    CHECK(copy_only.find("_Tp __") == std::string::npos);
    CHECK(copy_only.find("new") == std::string::npos);
    CHECK(copy_only.find("malloc") == std::string::npos);
    CHECK(copy_only.find("return 0") == std::string::npos);
    CHECK(copy_only.find("PROVED") == std::string::npos);
    CHECK(copy_only.find("CLEAN") == std::string::npos);
    CHECK(fill_body.find("This is not PROVED and it is not CLEAN.") != std::string::npos);
    CHECK(copy_body.find("This is not PROVED and it is not CLEAN.") != std::string::npos);

    const auto copy_bw_at =
        text.find("constexpr _Tp* copy_backward(_Tp* __first, _Tp* __last, _Tp* __dest)");
    REQUIRE(copy_bw_at != std::string::npos);
    CHECK(copy_n_at < copy_bw_at);
    const auto copy_n_sig = text.find("copy_n(", copy_n_at);
    REQUIRE(copy_n_sig != std::string::npos);
    CHECK(text.find("copy_n(", copy_n_sig + 7) == std::string::npos);
    const std::string copy_n_body = text.substr(copy_n_at, copy_bw_at - copy_n_at);
    const auto copy_n_brace = copy_n_body.find('{');
    REQUIRE(copy_n_brace != std::string::npos);
    const std::string copy_n_fn = copy_n_body.substr(copy_n_brace);
    const auto copy_n_empty = copy_n_fn.find("if (__n == 0) return __dest;");
    const auto copy_n_src = copy_n_fn.find("if (__first == nullptr)");
    const auto copy_n_dst = copy_n_fn.find("if (__dest == nullptr)");
    const auto copy_n_loop = copy_n_fn.find("while (__n != 0)");
    REQUIRE(copy_n_empty != std::string::npos);
    REQUIRE(copy_n_src != std::string::npos);
    REQUIRE(copy_n_dst != std::string::npos);
    REQUIRE(copy_n_loop != std::string::npos);
    CHECK(copy_n_empty < copy_n_src);
    CHECK(copy_n_src < copy_n_dst);
    CHECK(copy_n_dst < copy_n_loop);
    const std::string copy_n_src_body = copy_n_fn.substr(copy_n_src, copy_n_dst - copy_n_src);
    const auto copy_n_src_check = copy_n_src_body.find("__prism_check");
    const auto copy_n_src_trap = copy_n_src_body.find("__builtin_trap");
    REQUIRE(copy_n_src_check != std::string::npos);
    REQUIRE(copy_n_src_trap != std::string::npos);
    CHECK(copy_n_src_check < copy_n_src_trap);
    CHECK(copy_n_src_body.find("PTR-NULL-DEREF") != std::string::npos);
    CHECK(copy_n_src_body.find("std::copy_n null source") != std::string::npos);
    CHECK(copy_n_src_body.find("return") == std::string::npos);
    CHECK(copy_n_src_body.find("*__") == std::string::npos);
    const std::string copy_n_dst_body = copy_n_fn.substr(copy_n_dst, copy_n_loop - copy_n_dst);
    const auto copy_n_dst_check = copy_n_dst_body.find("__prism_check");
    const auto copy_n_dst_trap = copy_n_dst_body.find("__builtin_trap");
    REQUIRE(copy_n_dst_check != std::string::npos);
    REQUIRE(copy_n_dst_trap != std::string::npos);
    CHECK(copy_n_dst_check < copy_n_dst_trap);
    CHECK(copy_n_dst_body.find("PTR-NULL-DEREF") != std::string::npos);
    CHECK(copy_n_dst_body.find("std::copy_n null destination") != std::string::npos);
    CHECK(copy_n_dst_body.find("return") == std::string::npos);
    CHECK(copy_n_dst_body.find("*__") == std::string::npos);
    CHECK(copy_n_fn.substr(0, copy_n_loop).find("*__") == std::string::npos);
    const auto copy_n_store = copy_n_fn.find("*__out = *__first", copy_n_loop);
    REQUIRE(copy_n_store != std::string::npos);
    const auto copy_n_bump_first = copy_n_fn.find("++__first", copy_n_store);
    REQUIRE(copy_n_bump_first != std::string::npos);
    const auto copy_n_bump_out = copy_n_fn.find("++__out", copy_n_store);
    REQUIRE(copy_n_bump_out != std::string::npos);
    const auto copy_n_dec = copy_n_fn.find("--__n", copy_n_store);
    REQUIRE(copy_n_dec != std::string::npos);
    const auto copy_n_ret = copy_n_fn.find("return __out", copy_n_dec);
    REQUIRE(copy_n_ret != std::string::npos);
    CHECK(copy_n_loop < copy_n_store);
    CHECK(copy_n_store < copy_n_bump_first);
    CHECK(copy_n_bump_first < copy_n_bump_out);
    CHECK(copy_n_bump_out < copy_n_dec);
    CHECK(copy_n_dec < copy_n_ret);
    CHECK(copy_n_fn.find("*__out = *__first", copy_n_store + 1) == std::string::npos);
    CHECK(copy_n_fn.find("*__last") == std::string::npos);
    const auto copy_n_end = copy_n_body.rfind("\n}");
    REQUIRE(copy_n_end != std::string::npos);
    const std::string copy_n_only = copy_n_body.substr(0, copy_n_end);
    CHECK(copy_n_only.find("<=") == std::string::npos);
    CHECK(copy_n_only.find("_Tp{") == std::string::npos);
    CHECK(copy_n_only.find("_Tp(") == std::string::npos);
    CHECK(copy_n_only.find("_Tp __") == std::string::npos);
    CHECK(copy_n_only.find("new") == std::string::npos);
    CHECK(copy_n_only.find("malloc") == std::string::npos);
    CHECK(copy_n_only.find("return 0") == std::string::npos);
    CHECK(copy_n_only.find("PROVED") == std::string::npos);
    CHECK(copy_n_only.find("CLEAN") == std::string::npos);
    CHECK(copy_n_body.find("This is not PROVED and it is not CLEAN.") != std::string::npos);

    const auto copy_bw_sig = text.find("copy_backward(", copy_bw_at);
    REQUIRE(copy_bw_sig != std::string::npos);
    CHECK(text.find("copy_backward(", copy_bw_sig + 14) == std::string::npos);
    const auto copy_bw_stop = text.find("namespace ranges {", copy_bw_at);
    REQUIRE(copy_bw_stop != std::string::npos);
    const std::string copy_bw_body = text.substr(copy_bw_at, copy_bw_stop - copy_bw_at);
    const auto copy_bw_brace = copy_bw_body.find('{');
    REQUIRE(copy_bw_brace != std::string::npos);
    const std::string copy_bw_fn = copy_bw_body.substr(copy_bw_brace);
    const auto copy_bw_bad = copy_bw_fn.find("if (__last < __first)");
    const auto copy_bw_empty = copy_bw_fn.find("if (__first == __last) return __dest;");
    const auto copy_bw_null = copy_bw_fn.find("if (__dest == nullptr)");
    const auto copy_bw_scan = copy_bw_fn.find("while (__scan != __first)");
    const auto copy_bw_hit = copy_bw_fn.find("if (__scan == __dest)");
    const auto copy_bw_src = copy_bw_fn.find("_Tp* __src = __last;");
    const auto copy_bw_out = copy_bw_fn.find("_Tp* __out = __dest;");
    const auto copy_bw_loop = copy_bw_fn.find("while (__src != __first)");
    REQUIRE(copy_bw_bad != std::string::npos);
    REQUIRE(copy_bw_empty != std::string::npos);
    REQUIRE(copy_bw_null != std::string::npos);
    REQUIRE(copy_bw_scan != std::string::npos);
    REQUIRE(copy_bw_hit != std::string::npos);
    REQUIRE(copy_bw_src != std::string::npos);
    REQUIRE(copy_bw_out != std::string::npos);
    REQUIRE(copy_bw_loop != std::string::npos);
    CHECK(copy_bw_bad < copy_bw_empty);
    CHECK(copy_bw_empty < copy_bw_null);
    CHECK(copy_bw_null < copy_bw_scan);
    CHECK(copy_bw_scan < copy_bw_hit);
    CHECK(copy_bw_hit < copy_bw_src);
    // The scan starts at last and stops before first: (first, last].
    CHECK(prism_test::first_before(copy_bw_fn, "_Tp* __scan = __last;", copy_bw_scan));
    const std::string copy_bw_scan_body = copy_bw_fn.substr(copy_bw_scan, copy_bw_src - copy_bw_scan);
    const auto copy_bw_scan_check = copy_bw_scan_body.find("__prism_check");
    const auto copy_bw_scan_trap = copy_bw_scan_body.find("__builtin_trap");
    REQUIRE(copy_bw_scan_check != std::string::npos);
    REQUIRE(copy_bw_scan_trap != std::string::npos);
    CHECK(copy_bw_scan_check < copy_bw_scan_trap);
    CHECK(prism_test::first_after(copy_bw_scan_body, "--__scan", copy_bw_scan_trap));
    CHECK(copy_bw_scan_body.find("*__") == std::string::npos);
    CHECK(copy_bw_scan_body.find("<") == std::string::npos);
    CHECK(copy_bw_scan_body.find("return") == std::string::npos);
    CHECK(copy_bw_scan_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(copy_bw_scan_body.find("std::copy_backward dest lies in (first, last]") != std::string::npos);
    CHECK(copy_bw_null < copy_bw_src);
    CHECK(copy_bw_src < copy_bw_out);
    CHECK(copy_bw_out < copy_bw_loop);
    const std::string copy_bw_bad_body = copy_bw_fn.substr(copy_bw_bad, copy_bw_empty - copy_bw_bad);
    const auto copy_bw_bad_check = copy_bw_bad_body.find("__prism_check");
    const auto copy_bw_bad_trap = copy_bw_bad_body.find("__builtin_trap");
    REQUIRE(copy_bw_bad_check != std::string::npos);
    REQUIRE(copy_bw_bad_trap != std::string::npos);
    CHECK(copy_bw_bad_check < copy_bw_bad_trap);
    CHECK(copy_bw_bad_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(copy_bw_bad_body.find("std::copy_backward last is before first") != std::string::npos);
    CHECK(copy_bw_bad_body.find("return") == std::string::npos);
    CHECK(copy_bw_bad_body.find("*__") == std::string::npos);
    CHECK(copy_bw_bad_body.find("__dest") == std::string::npos);
    const std::string copy_bw_null_body = copy_bw_fn.substr(copy_bw_null, copy_bw_scan - copy_bw_null);
    const auto copy_bw_null_check = copy_bw_null_body.find("__prism_check");
    const auto copy_bw_null_trap = copy_bw_null_body.find("__builtin_trap");
    REQUIRE(copy_bw_null_check != std::string::npos);
    REQUIRE(copy_bw_null_trap != std::string::npos);
    CHECK(copy_bw_null_check < copy_bw_null_trap);
    CHECK(copy_bw_null_body.find("PTR-NULL-DEREF") != std::string::npos);
    CHECK(copy_bw_null_body.find("std::copy_backward null destination") != std::string::npos);
    CHECK(copy_bw_null_body.find("return") == std::string::npos);
    CHECK(copy_bw_null_body.find("*__") == std::string::npos);
    CHECK(copy_bw_fn.substr(0, copy_bw_loop).find("*__") == std::string::npos);
    const auto copy_bw_dec_src = copy_bw_fn.find("--__src", copy_bw_loop);
    REQUIRE(copy_bw_dec_src != std::string::npos);
    const auto copy_bw_dec_out = copy_bw_fn.find("--__out", copy_bw_dec_src);
    REQUIRE(copy_bw_dec_out != std::string::npos);
    const auto copy_bw_store = copy_bw_fn.find("*__out = *__src", copy_bw_dec_out);
    REQUIRE(copy_bw_store != std::string::npos);
    const auto copy_bw_ret = copy_bw_fn.find("return __out", copy_bw_store);
    REQUIRE(copy_bw_ret != std::string::npos);
    CHECK(copy_bw_loop < copy_bw_dec_src);
    CHECK(copy_bw_dec_src < copy_bw_dec_out);
    CHECK(copy_bw_dec_out < copy_bw_store);
    CHECK(copy_bw_store < copy_bw_ret);
    CHECK(copy_bw_fn.find("*__out = *__src", copy_bw_store + 1) == std::string::npos);
    CHECK(copy_bw_fn.find("*__last") == std::string::npos);
    CHECK(copy_bw_fn.find("*__dest") == std::string::npos);
    CHECK(copy_bw_fn.find("++") == std::string::npos);
    const auto copy_bw_end = copy_bw_body.rfind("\n}");
    REQUIRE(copy_bw_end != std::string::npos);
    const std::string copy_bw_only = copy_bw_body.substr(0, copy_bw_end);
    CHECK(copy_bw_only.find("<=") == std::string::npos);
    CHECK(copy_bw_only.find("_Tp{") == std::string::npos);
    CHECK(copy_bw_only.find("_Tp(") == std::string::npos);
    CHECK(copy_bw_only.find("_Tp __") == std::string::npos);
    CHECK(copy_bw_only.find("new") == std::string::npos);
    CHECK(copy_bw_only.find("malloc") == std::string::npos);
    CHECK(copy_bw_only.find("return 0") == std::string::npos);
    CHECK(copy_bw_only.find("PROVED") == std::string::npos);
    CHECK(copy_bw_only.find("CLEAN") == std::string::npos);

    const std::string ci_sig =
        "constexpr __PTRDIFF_TYPE__ count_if(_Tp (&__a)[_Nm],\n"
        "                                    bool (*__pred)(const typename _Prism_count_if_identity<_Tp>::type&))";
    const std::string ci_zero_sig = "constexpr __PTRDIFF_TYPE__ count_if(_Arr&, bool (*)(const _Elem&)) noexcept";
    const auto ci_at = text.find(ci_sig);
    REQUIRE(ci_at != std::string::npos);
    CHECK(text.find(ci_sig, ci_at + 1) == std::string::npos);
    CHECK(prism_test::first_before(text, "namespace ranges {", ci_at));
    // No zero-length overload: libstdc++ 14 under clang 23 rejects count_if on int[0].
    CHECK(text.find(ci_zero_sig) == std::string::npos);
    CHECK(text.find("sizeof(_Arr) == 0") == std::string::npos);
    const auto ci_end = text.find("}  // namespace ranges", ci_at);
    REQUIRE(ci_end != std::string::npos);
    const std::string ci_fn = text.substr(ci_at, ci_end - ci_at);
    const auto ci_null = ci_fn.find("if (__pred == nullptr)");
    const auto ci_check = ci_fn.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::ranges::count_if null predicate\")");
    const auto ci_trap = ci_fn.find("__builtin_trap()");
    const auto ci_scan = ci_fn.find("for (size_t __i = 0; __i < _Nm; ++__i)");
    const auto ci_call = ci_fn.find("__pred(__a[__i])");
    const auto ci_inc = ci_fn.find("++__n");
    const auto ci_ret = ci_fn.find("return __n;");
    REQUIRE(ci_null != std::string::npos);
    REQUIRE(ci_check != std::string::npos);
    REQUIRE(ci_trap != std::string::npos);
    REQUIRE(ci_scan != std::string::npos);
    REQUIRE(ci_call != std::string::npos);
    REQUIRE(ci_inc != std::string::npos);
    REQUIRE(ci_ret != std::string::npos);
    CHECK(ci_null < ci_check);
    CHECK(ci_check < ci_trap);
    CHECK(ci_trap < ci_scan);
    CHECK(ci_scan < ci_call);
    CHECK(ci_call < ci_inc);
    CHECK(ci_inc < ci_ret);
    const std::string ci_pre = ci_fn.substr(0, ci_scan);
    const std::string ci_pre_body = ci_pre.substr(ci_pre.find('{'));
    CHECK(ci_pre_body.find("__pred(") == std::string::npos);
    CHECK(ci_pre_body.find("__a[") == std::string::npos);
    CHECK(ci_pre_body.find("return") == std::string::npos);
    const std::string ci_body = ci_fn.substr(ci_fn.find('{'));
    CHECK(ci_body.find("[_Nm]") == std::string::npos);
    CHECK(ci_body.find("__i <= _Nm") == std::string::npos);
    CHECK(ci_body.find("return __a") == std::string::npos);
    CHECK(ci_body.find("*__") == std::string::npos);
    CHECK(ci_body.find("new") == std::string::npos);
    const auto ci_ret_kw = ci_body.find("return ");
    REQUIRE(ci_ret_kw != std::string::npos);
    CHECK(ci_body.find("return ", ci_ret_kw + 1) == std::string::npos);
    const auto ci_msg = text.find("\"std::ranges::count_if null predicate\")");
    REQUIRE(ci_msg != std::string::npos);
    CHECK(text.find("\"std::ranges::count_if null predicate\")", ci_msg + 1) == std::string::npos);

}

TEST_CASE("cxx algorithm model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxalgorithm-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx algorithm model: trace matches libstdc++ and an inverted clamp does not return v") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "algorithm_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(model_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(has_line(b, "min_eq=1"));
    CHECK(has_line(b, "max_eq=1"));
    CHECK(has_line(b, "min_lo=2"));
    CHECK(has_line(b, "max_hi=9"));
    CHECK(has_line(b, "clamp_mid=5 id=1"));
    CHECK(has_line(b, "clamp_lo=2 id=1"));
    CHECK(has_line(b, "clamp_hi=9 id=1"));
    CHECK(has_line(b, "wide_min_is0=0 wide_min_is_min=1"));
    CHECK(has_line(b, "wide_max_is0=0 wide_max_is_max=1"));
    CHECK(has_line(b, "clamp_inside_zero=1"));
    CHECK(has_line(b, "ord_eq_first=1 ord_max=2"));
    CHECK(has_line(b, "ptr_eq_first=1"));
    CHECK(has_line(b, "el_min=1 el_min_i=2"));
    CHECK(has_line(b, "el_max=9 el_max_i=0"));
    CHECK(has_line(b, "el_empty=1"));
    CHECK(has_line(b, "el_tie=1"));
    CHECK(has_line(b, "el_one=1"));
    CHECK(has_line(b, "el_null=1"));
    CHECK(has_line(b, "el_ord_min=1 el_ord_max_i=0"));
    CHECK(has_line(b, "el_nodef_min=1 el_nodef_max_i=0 el_nodef_empty=1"));
    CHECK(has_line(b, "find_hit=1 find_hit_i=2"));
    CHECK(has_line(b, "find_first_i=1"));
    CHECK(has_line(b, "find_front_i=0"));
    CHECK(has_line(b, "find_miss=1"));
    CHECK(has_line(b, "find_empty=1"));
    CHECK(has_line(b, "find_one=1"));
    CHECK(has_line(b, "find_null=1"));
    CHECK(has_line(b, "find_keep=1"));
    CHECK(has_line(b, "find_eq_i=1 find_eq_id=1 find_eq_miss=1 find_eq_empty=1"));
    CHECK(has_line(b, "find_nodef=1 find_nodef_i=1 find_nodef_empty=1"));
    CHECK(has_line(b, "find_if_hit=1 find_if_hit_i=2 find_if_hit_calls=3"));
    CHECK(has_line(b, "find_if_first_i=1 find_if_first_calls=2"));
    CHECK(has_line(b, "find_if_front_i=0 find_if_front_calls=1"));
    CHECK(has_line(b, "find_if_miss=1 find_if_miss_calls=4"));
    CHECK(has_line(b, "find_if_empty=1 find_if_empty_calls=0"));
    CHECK(has_line(b, "find_if_tie=1 find_if_tie_calls=1"));
    CHECK(has_line(b, "find_if_one=1"));
    CHECK(has_line(b, "find_if_null=1"));
    CHECK(has_line(b, "find_if_keep=1"));
    CHECK(has_line(b, "find_if_eq_i=1 find_if_eq_empty=1"));
    CHECK(has_line(b, "find_if_nodef_i=1 find_if_nodef_empty=1"));
    CHECK(has_line(b, "find_if_pred_i=1 find_if_pred_empty=1"));
    CHECK(has_line(b, "count_four=2"));
    CHECK(has_line(b, "count_one=1"));
    CHECK(has_line(b, "count_nine=1"));
    CHECK(has_line(b, "count_miss=0"));
    CHECK(has_line(b, "count_empty=0"));
    CHECK(has_line(b, "count_tie=2"));
    CHECK(has_line(b, "count_one_hit=1"));
    CHECK(has_line(b, "count_one_miss=0"));
    CHECK(has_line(b, "count_null=0"));
    CHECK(has_line(b, "count_eq=2 count_eq_miss=0 count_eq_empty=0"));
    CHECK(has_line(b, "count_nodef=1 count_nodef_three=2 count_nodef_empty=0"));
    CHECK(has_line(b, "count_keep=1"));
    CHECK(has_line(b, "all_of_all=1 all_of_all_calls=4"));
    CHECK(has_line(b, "all_of_front=0 all_of_front_calls=1"));
    CHECK(has_line(b, "all_of_mid=0 all_of_mid_calls=2"));
    CHECK(has_line(b, "all_of_empty=1 all_of_empty_calls=0"));
    CHECK(has_line(b, "all_of_one=1"));
    CHECK(has_line(b, "all_of_null=1 all_of_null_calls=0"));
    CHECK(has_line(b, "all_of_keep=1"));
    CHECK(has_line(b, "all_of_eq=0 all_of_eq_empty=1"));
    CHECK(has_line(b, "all_of_nodef=0 all_of_nodef_empty=1"));
    CHECK(has_line(b, "all_of_pred=0 all_of_pred_empty=1"));
    CHECK(has_line(b, "any_of_hit=1 any_of_hit_calls=3"));
    CHECK(has_line(b, "any_of_front=1 any_of_front_calls=1"));
    CHECK(has_line(b, "any_of_miss=0 any_of_miss_calls=4"));
    CHECK(has_line(b, "any_of_empty=0 any_of_empty_calls=0"));
    CHECK(has_line(b, "any_of_tie=1 any_of_tie_calls=1"));
    CHECK(has_line(b, "any_of_one=1"));
    CHECK(has_line(b, "any_of_null=0 any_of_null_calls=0"));
    CHECK(has_line(b, "any_of_keep=1"));
    CHECK(has_line(b, "any_of_eq=1 any_of_eq_empty=0"));
    CHECK(has_line(b, "any_of_nodef=1 any_of_nodef_empty=0"));
    CHECK(has_line(b, "any_of_pred=1 any_of_pred_empty=0"));
    CHECK(has_line(b, "none_of_miss=1 none_of_miss_calls=4"));
    CHECK(has_line(b, "none_of_front=0 none_of_front_calls=1"));
    CHECK(has_line(b, "none_of_mid=0 none_of_mid_calls=3"));
    CHECK(has_line(b, "none_of_empty=1 none_of_empty_calls=0"));
    CHECK(has_line(b, "none_of_one=1"));
    CHECK(has_line(b, "none_of_null=1 none_of_null_calls=0"));
    CHECK(has_line(b, "none_of_keep=1"));
    CHECK(has_line(b, "none_of_eq=0 none_of_eq_empty=1"));
    CHECK(has_line(b, "none_of_nodef=0 none_of_nodef_empty=1"));
    CHECK(has_line(b, "none_of_pred=0 none_of_pred_empty=1"));
    CHECK(has_line(b, "fill_all=7,7,7,7"));
    CHECK(has_line(b, "fill_span=7,7,7,4"));
    CHECK(has_line(b, "fill_empty=9,4,1,4"));
    CHECK(has_line(b, "fill_one=3"));
    CHECK(has_line(b, "fill_null=1"));
    CHECK(has_line(b, "fill_pred=9,9,9 fill_pred_empty=4,5"));
    CHECK(has_line(b, "copy_all=9,4,1,4 end=1 src=9,4,1,4"));
    CHECK(has_line(b, "copy_span=9,4,1,8 end=1"));
    CHECK(has_line(b, "copy_empty=8,7,6,5 end=1"));
    CHECK(has_line(b, "copy_one=8 end=1"));
    CHECK(has_line(b, "copy_null=1"));
    CHECK(has_line(b, "copy_pred=3,1,3,5 copy_pred_empty=4,5"));
    CHECK(has_line(b, "copy_adj=1,2,3,1,2,3 end=1"));
    CHECK(has_line(b, "copy_n_all=9,4,1,4 end=1 src=9,4,1,4"));
    CHECK(has_line(b, "copy_n_span=9,4,1,8 end=1"));
    CHECK(has_line(b, "copy_n_empty=8,7,6,5 end=1"));
    CHECK(has_line(b, "copy_n_one=8 end=1"));
    CHECK(has_line(b, "copy_n_null=1"));
    CHECK(has_line(b, "copy_n_null_src=8,7,6,5 end=1"));
    CHECK(has_line(b, "copy_n_null_dst=1 src=9,4,1,4"));
    CHECK(has_line(b, "copy_n_pred=3,1,3,5 copy_n_pred_empty=4,5"));
    CHECK(has_line(b, "copy_n_adj=1,2,3,1,2,3 end=1"));
    CHECK(has_line(b, "copy_backward_all=9,4,1,4 end=1 src=9,4,1,4"));
    CHECK(has_line(b, "copy_backward_span=9,4,1,8 end=1"));
    CHECK(has_line(b, "copy_backward_empty=8,7,6,5 end=1"));
    CHECK(has_line(b, "copy_backward_one=8 end=1"));
    CHECK(has_line(b, "copy_backward_null=1"));
    CHECK(has_line(b, "copy_backward_null_src=8,7,6,5 end=1"));
    CHECK(has_line(b, "copy_backward_null_dst=1 src=9,4,1,4"));
    CHECK(has_line(b, "copy_backward_pred=3,1,3,5 copy_backward_pred_empty=4,5"));
    CHECK(has_line(b, "copy_backward_adj=1,2,3,1,2,3 end=1"));
    CHECK(has_line(b, "copy_backward_shift=1,1,2,3 end=1"));

    std::string out, err;
    CHECK_FALSE(run_cmd({mod.string(), "invert"}, &out, &err));
    const std::string both = out + err;
    CHECK(both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(both.find("clamp: hi < lo") != std::string::npos);
    CHECK(out.find("value=") == std::string::npos);

    for (const char* arg : {"invert-min", "invert-max"}) {
        std::string el_out, el_err;
        CHECK_FALSE(run_cmd({mod.string(), arg}, &el_out, &el_err));
        const std::string el_both = el_out + el_err;
        const char* el_msg =
            std::string(arg) == "invert-min" ? "min_element: last < first" : "max_element: last < first";
        CHECK(el_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
        CHECK(el_both.find(el_msg) != std::string::npos);
        CHECK(el_out.find("value=") == std::string::npos);
    }

    std::string find_out, find_err;
    CHECK_FALSE(run_cmd({mod.string(), "invert-find"}, &find_out, &find_err));
    const std::string find_both = find_out + find_err;
    CHECK(find_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(find_both.find("find: last < first") != std::string::npos);
    CHECK(find_out.find("value=") == std::string::npos);

    std::string fi_out, fi_err;
    CHECK_FALSE(run_cmd({mod.string(), "invert-find-if"}, &fi_out, &fi_err));
    const std::string fi_both = fi_out + fi_err;
    CHECK(fi_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(fi_both.find("find_if: last < first") != std::string::npos);
    CHECK(fi_both.find("called") == std::string::npos);
    CHECK(fi_out.find("value=") == std::string::npos);

    std::string null_out, null_err;
    CHECK_FALSE(run_cmd({mod.string(), "null-find-if"}, &null_out, &null_err));
    const std::string null_both = null_out + null_err;
    CHECK(null_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(null_both.find("find_if: null predicate") != std::string::npos);
    CHECK(null_out.find("value=") == std::string::npos);

    std::string null_empty_out, null_empty_err;
    CHECK_FALSE(run_cmd({mod.string(), "null-find-if-empty"}, &null_empty_out, &null_empty_err));
    const std::string null_empty_both = null_empty_out + null_empty_err;
    CHECK(null_empty_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(null_empty_both.find("find_if: null predicate") != std::string::npos);
    CHECK(null_empty_out.find("value=") == std::string::npos);

    std::string count_out, count_err;
    CHECK_FALSE(run_cmd({mod.string(), "invert-count"}, &count_out, &count_err));
    const std::string count_both = count_out + count_err;
    CHECK(count_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(count_both.find("count: last < first") != std::string::npos);
    CHECK(count_out.find("value=") == std::string::npos);

    const auto expect_contract = [&](const char* arg, const char* msg, bool no_called) {
        std::string o, e;
        CHECK_FALSE(run_cmd({mod.string(), arg}, &o, &e));
        const std::string both = o + e;
        CHECK(both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
        CHECK(both.find(msg) != std::string::npos);
        CHECK(o.find("value=") == std::string::npos);
        if (no_called) CHECK(both.find("called") == std::string::npos);
    };
    expect_contract("invert-all-of", "all_of: last < first", true);
    expect_contract("invert-any-of", "any_of: last < first", true);
    expect_contract("invert-none-of", "none_of: last < first", true);
    expect_contract("null-all-of", "all_of: null predicate", false);
    expect_contract("null-all-of-empty", "all_of: null predicate", false);
    expect_contract("null-any-of", "any_of: null predicate", false);
    expect_contract("null-any-of-empty", "any_of: null predicate", false);
    expect_contract("null-none-of", "none_of: null predicate", false);
    expect_contract("null-none-of-empty", "none_of: null predicate", false);

    std::string fill_out, fill_err;
    CHECK_FALSE(run_cmd({mod.string(), "invert-fill"}, &fill_out, &fill_err));
    const std::string fill_both = fill_out + fill_err;
    CHECK(fill_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(fill_both.find("std::fill last is before first") != std::string::npos);
    CHECK(fill_both.find("wrote") == std::string::npos);
    CHECK(fill_out.find("value=") == std::string::npos);

    std::string null_fill_out, null_fill_err;
    CHECK_FALSE(run_cmd({mod.string(), "null-fill"}, &null_fill_out, &null_fill_err));
    const std::string null_fill_both = null_fill_out + null_fill_err;
    CHECK(null_fill_both.find("CHECK PTR-NULL-DEREF:") != std::string::npos);
    CHECK(null_fill_both.find("std::fill null destination") != std::string::npos);
    CHECK(null_fill_both.find("wrote") == std::string::npos);
    CHECK(null_fill_both.find("AddressSanitizer") == std::string::npos);
    CHECK(null_fill_out.find("value=") == std::string::npos);

    std::string copy_out, copy_err;
    CHECK_FALSE(run_cmd({mod.string(), "invert-copy"}, &copy_out, &copy_err));
    const std::string copy_both = copy_out + copy_err;
    CHECK(copy_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(copy_both.find("std::copy last is before first") != std::string::npos);
    CHECK(copy_both.find("wrote") == std::string::npos);
    CHECK(copy_both.find("read") == std::string::npos);
    CHECK(copy_out.find("value=") == std::string::npos);

    std::string null_copy_out, null_copy_err;
    CHECK_FALSE(run_cmd({mod.string(), "null-copy"}, &null_copy_out, &null_copy_err));
    const std::string null_copy_both = null_copy_out + null_copy_err;
    CHECK(null_copy_both.find("CHECK PTR-NULL-DEREF:") != std::string::npos);
    CHECK(null_copy_both.find("std::copy null destination") != std::string::npos);
    CHECK(null_copy_both.find("wrote") == std::string::npos);
    CHECK(null_copy_both.find("read") == std::string::npos);
    CHECK(null_copy_both.find("AddressSanitizer") == std::string::npos);
    CHECK(null_copy_out.find("value=") == std::string::npos);

    std::string overlap_out, overlap_err;
    CHECK_FALSE(run_cmd({mod.string(), "overlap-copy"}, &overlap_out, &overlap_err));
    const std::string overlap_both = overlap_out + overlap_err;
    CHECK(overlap_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(overlap_both.find("std::copy dest lies in [first, last)") != std::string::npos);
    CHECK(overlap_both.find("wrote") == std::string::npos);
    CHECK(overlap_both.find("read") == std::string::npos);
    CHECK(overlap_out.find("value=") == std::string::npos);

    std::string overlap_first_out, overlap_first_err;
    CHECK_FALSE(run_cmd({mod.string(), "overlap-copy-first"}, &overlap_first_out, &overlap_first_err));
    const std::string overlap_first_both = overlap_first_out + overlap_first_err;
    CHECK(overlap_first_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(overlap_first_both.find("std::copy dest lies in [first, last)") != std::string::npos);
    CHECK(overlap_first_both.find("wrote") == std::string::npos);
    CHECK(overlap_first_both.find("read") == std::string::npos);
    CHECK(overlap_first_out.find("value=") == std::string::npos);

    std::string null_copy_n_src_out, null_copy_n_src_err;
    CHECK_FALSE(run_cmd({mod.string(), "null-copy-n-source"}, &null_copy_n_src_out, &null_copy_n_src_err));
    const std::string null_copy_n_src_both = null_copy_n_src_out + null_copy_n_src_err;
    CHECK(null_copy_n_src_both.find("CHECK PTR-NULL-DEREF:") != std::string::npos);
    CHECK(null_copy_n_src_both.find("std::copy_n null source") != std::string::npos);
    CHECK(null_copy_n_src_both.find("std::copy_n null destination") == std::string::npos);
    CHECK(null_copy_n_src_both.find("wrote") == std::string::npos);
    CHECK(null_copy_n_src_both.find("read") == std::string::npos);
    CHECK(null_copy_n_src_both.find("AddressSanitizer") == std::string::npos);
    CHECK(null_copy_n_src_out.find("value=") == std::string::npos);

    std::string null_copy_n_dst_out, null_copy_n_dst_err;
    CHECK_FALSE(run_cmd({mod.string(), "null-copy-n-dest"}, &null_copy_n_dst_out, &null_copy_n_dst_err));
    const std::string null_copy_n_dst_both = null_copy_n_dst_out + null_copy_n_dst_err;
    CHECK(null_copy_n_dst_both.find("CHECK PTR-NULL-DEREF:") != std::string::npos);
    CHECK(null_copy_n_dst_both.find("std::copy_n null destination") != std::string::npos);
    CHECK(null_copy_n_dst_both.find("std::copy_n null source") == std::string::npos);
    CHECK(null_copy_n_dst_both.find("wrote") == std::string::npos);
    CHECK(null_copy_n_dst_both.find("read") == std::string::npos);
    CHECK(null_copy_n_dst_both.find("AddressSanitizer") == std::string::npos);
    CHECK(null_copy_n_dst_out.find("value=") == std::string::npos);

    std::string null_copy_n_both_out, null_copy_n_both_err;
    CHECK_FALSE(run_cmd({mod.string(), "null-copy-n-both"}, &null_copy_n_both_out, &null_copy_n_both_err));
    const std::string null_copy_n_both_both = null_copy_n_both_out + null_copy_n_both_err;
    CHECK(null_copy_n_both_both.find("CHECK PTR-NULL-DEREF:") != std::string::npos);
    CHECK(null_copy_n_both_both.find("std::copy_n null source") != std::string::npos);
    CHECK(null_copy_n_both_both.find("std::copy_n null destination") == std::string::npos);
    CHECK(null_copy_n_both_both.find("wrote") == std::string::npos);
    CHECK(null_copy_n_both_both.find("read") == std::string::npos);
    CHECK(null_copy_n_both_both.find("AddressSanitizer") == std::string::npos);
    CHECK(null_copy_n_both_out.find("value=") == std::string::npos);

    std::string copy_bw_out, copy_bw_err;
    CHECK_FALSE(run_cmd({mod.string(), "invert-copy-backward"}, &copy_bw_out, &copy_bw_err));
    const std::string copy_bw_both = copy_bw_out + copy_bw_err;
    CHECK(copy_bw_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(copy_bw_both.find("std::copy_backward last is before first") != std::string::npos);
    CHECK(copy_bw_both.find("wrote") == std::string::npos);
    CHECK(copy_bw_both.find("read") == std::string::npos);
    CHECK(copy_bw_out.find("value=") == std::string::npos);

    std::string null_copy_bw_out, null_copy_bw_err;
    CHECK_FALSE(run_cmd({mod.string(), "null-copy-backward"}, &null_copy_bw_out, &null_copy_bw_err));
    const std::string null_copy_bw_both = null_copy_bw_out + null_copy_bw_err;
    CHECK(null_copy_bw_both.find("CHECK PTR-NULL-DEREF:") != std::string::npos);
    CHECK(null_copy_bw_both.find("std::copy_backward null destination") != std::string::npos);
    CHECK(null_copy_bw_both.find("wrote") == std::string::npos);
    CHECK(null_copy_bw_both.find("read") == std::string::npos);
    CHECK(null_copy_bw_both.find("AddressSanitizer") == std::string::npos);
    CHECK(null_copy_bw_out.find("value=") == std::string::npos);

    for (const char* arg : {"overlap-copy-backward", "overlap-copy-backward-last"}) {
        std::string ov_out, ov_err;
        CHECK_FALSE(run_cmd({mod.string(), arg}, &ov_out, &ov_err));
        const std::string ov_both = ov_out + ov_err;
        CHECK(ov_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
        CHECK(ov_both.find("std::copy_backward dest lies in (first, last]") != std::string::npos);
        CHECK(ov_both.find("wrote") == std::string::npos);
        CHECK(ov_both.find("read") == std::string::npos);
        CHECK(ov_out.find("value=") == std::string::npos);
    }

    const fs::path omitted = c.tmp / "omitted.cpp";
    std::ofstream(omitted) << "#include <algorithm>\n"
                                  "int use(int* a, int n) {\n"
                                  "  std::sort(a, a + n);\n"
                                  "  std::for_each(a, a + n, [](int) {});\n"
                                  "  return a[0];\n"
                                  "}\n";
    std::string omitted_err;
    CHECK(syntax_only(c.cxx, omitted, false, &omitted_err));
    CHECK_FALSE(syntax_only(c.cxx, omitted, true, &omitted_err));

    const fs::path comp = c.tmp / "comp.cpp";
    std::ofstream(comp) << "#include <algorithm>\n"
                           "int use(int* a, int n) {\n"
                           "  auto less = [](int x, int y) { return x < y; };\n"
                           "  return *std::min_element(a, a + n, less) +\n"
                           "         *std::max_element(a, a + n, less);\n"
                           "}\n";
    std::string comp_err;
    CHECK(syntax_only(c.cxx, comp, false, &comp_err));
    CHECK_FALSE(syntax_only(c.cxx, comp, true, &comp_err));

    const fs::path inverted = c.tmp / "invert_const.cpp";
    std::ofstream(inverted) << "#include <algorithm>\n"
                               "static_assert(std::clamp(0, 2, 1) == 0);\n";
    std::string inverted_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted, true, &inverted_err));

    const fs::path inverted_el = c.tmp / "invert_el_const.cpp";
    std::ofstream(inverted_el) << "#include <algorithm>\n"
                                  "constexpr int a[] = {1, 2, 3};\n"
                                  "static_assert(std::min_element(a + 2, a) == a);\n"
                                  "static_assert(std::max_element(a + 2, a) == a);\n";
    std::string inverted_el_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_el, true, &inverted_el_err));

    const fs::path find_ok = c.tmp / "find_ok.cpp";
    std::ofstream(find_ok) << "#include <algorithm>\n"
                                  "int use(int* a, int n, const int& v) {\n"
                                  "  int* p = std::find(a, a + n, v);\n"
                                  "  if (p == a + n) return 0;\n"
                                  "  return *p;\n"
                                  "}\n";
    std::string find_ok_err;
    CHECK(syntax_only(c.cxx, find_ok, false, &find_ok_err));
    CHECK(syntax_only(c.cxx, find_ok, true, &find_ok_err));

    const fs::path find_long = c.tmp / "find_long.cpp";
    std::ofstream(find_long) << "#include <algorithm>\n"
                                "long use(int* a, int n) {\n"
                                "  return *std::find(a, a + n, 0L);\n"
                                "}\n";
    std::string find_long_err;
    CHECK(syntax_only(c.cxx, find_long, false, &find_long_err));
    CHECK_FALSE(syntax_only(c.cxx, find_long, true, &find_long_err));

    const fs::path find_const = c.tmp / "find_const.cpp";
    std::ofstream(find_const) << "#include <algorithm>\n"
                                 "int use(const int* a, int n, int v) {\n"
                                 "  return *std::find(a, a + n, v);\n"
                                 "}\n";
    std::string find_const_err;
    CHECK(syntax_only(c.cxx, find_const, false, &find_const_err));
    CHECK_FALSE(syntax_only(c.cxx, find_const, true, &find_const_err));

    const fs::path find_if = c.tmp / "find_if.cpp";
    std::ofstream(find_if) << "#include <algorithm>\n"
                                  "int use(int* a, int n) {\n"
                                  "  return *std::find_if(a, a + n, [](int x) { return x == 0; });\n"
                                  "}\n";
    std::string find_if_err;
    CHECK(syntax_only(c.cxx, find_if, false, &find_if_err));
    CHECK_FALSE(syntax_only(c.cxx, find_if, true, &find_if_err));

    const fs::path inverted_find = c.tmp / "invert_find_const.cpp";
    std::ofstream(inverted_find) << "#include <algorithm>\n"
                                    "constexpr int inverted() {\n"
                                    "  int a[] = {1, 2, 3};\n"
                                    "  return *std::find(a + 2, a, 1);\n"
                                    "}\n"
                                    "static_assert(inverted() == 0);\n";
    std::string inverted_find_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_find, true, &inverted_find_err));

    const fs::path find_if_fn = c.tmp / "find_if_fn.cpp";
    std::ofstream(find_if_fn) << "#include <algorithm>\n"
                                 "bool is_zero(const int& x) { return x == 0; }\n"
                                 "int use(int* a, int n) {\n"
                                 "  int* p = std::find_if(a, a + n, is_zero);\n"
                                 "  if (p == a + n) return 0;\n"
                                 "  return *p;\n"
                                 "}\n";
    std::string find_if_fn_err;
    CHECK(syntax_only(c.cxx, find_if_fn, false, &find_if_fn_err));
    CHECK(syntax_only(c.cxx, find_if_fn, true, &find_if_fn_err));

    const fs::path find_if_val = c.tmp / "find_if_val.cpp";
    std::ofstream(find_if_val) << "#include <algorithm>\n"
                                  "bool is_zero(int x) { return x == 0; }\n"
                                  "int use(int* a, int n) {\n"
                                  "  return *std::find_if(a, a + n, is_zero);\n"
                                  "}\n";
    std::string find_if_val_err;
    CHECK(syntax_only(c.cxx, find_if_val, false, &find_if_val_err));
    CHECK_FALSE(syntax_only(c.cxx, find_if_val, true, &find_if_val_err));

    const fs::path count_ok = c.tmp / "count_ok.cpp";
    std::ofstream(count_ok) << "#include <algorithm>\n"
                               "#include <cstddef>\n"
                               "std::ptrdiff_t use(int* a, int n, const int& v) {\n"
                               "  return std::count(a, a + n, v);\n"
                               "}\n";
    std::string count_ok_err;
    CHECK(syntax_only(c.cxx, count_ok, false, &count_ok_err));
    CHECK(syntax_only(c.cxx, count_ok, true, &count_ok_err));

    const fs::path count_long = c.tmp / "count_long.cpp";
    std::ofstream(count_long) << "#include <algorithm>\n"
                                 "long use(int* a, int n) {\n"
                                 "  return std::count(a, a + n, 0L);\n"
                                 "}\n";
    std::string count_long_err;
    CHECK(syntax_only(c.cxx, count_long, false, &count_long_err));
    CHECK_FALSE(syntax_only(c.cxx, count_long, true, &count_long_err));

    const fs::path count_const = c.tmp / "count_const.cpp";
    std::ofstream(count_const) << "#include <algorithm>\n"
                                  "int use(const int* a, int n, int v) {\n"
                                  "  return static_cast<int>(std::count(a, a + n, v));\n"
                                  "}\n";
    std::string count_const_err;
    CHECK(syntax_only(c.cxx, count_const, false, &count_const_err));
    CHECK_FALSE(syntax_only(c.cxx, count_const, true, &count_const_err));

    const fs::path count_if = c.tmp / "count_if.cpp";
    std::ofstream(count_if) << "#include <algorithm>\n"
                               "int use(int* a, int n) {\n"
                               "  return static_cast<int>(std::count_if(a, a + n, [](int x) { return x == 0; }));\n"
                               "}\n";
    std::string count_if_err;
    CHECK(syntax_only(c.cxx, count_if, false, &count_if_err));
    CHECK_FALSE(syntax_only(c.cxx, count_if, true, &count_if_err));

    const fs::path count_empty_ok = c.tmp / "count_empty_ok.cpp";
    std::ofstream(count_empty_ok) << "#include <algorithm>\n"
                                     "constexpr int empty() {\n"
                                     "  int a[] = {1, 2, 3};\n"
                                     "  return static_cast<int>(std::count(a, a, 1));\n"
                                     "}\n"
                                     "static_assert(empty() == 0);\n";
    std::string count_empty_ok_err;
    CHECK(syntax_only(c.cxx, count_empty_ok, false, &count_empty_ok_err));
    CHECK(syntax_only(c.cxx, count_empty_ok, true, &count_empty_ok_err));

    const fs::path inverted_find_if = c.tmp / "invert_find_if_const.cpp";
    std::ofstream(inverted_find_if) << "#include <algorithm>\n"
                                       "constexpr bool is_one(const int& x) { return x == 1; }\n"
                                       "constexpr int inverted() {\n"
                                       "  int a[] = {1, 2, 3};\n"
                                       "  return *std::find_if(a + 2, a, is_one);\n"
                                       "}\n"
                                       "static_assert(inverted() == 0);\n";
    std::string inverted_find_if_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_find_if, true, &inverted_find_if_err));

    const fs::path null_find_if = c.tmp / "null_find_if_const.cpp";
    std::ofstream(null_find_if) << "#include <algorithm>\n"
                                   "constexpr int nullp() {\n"
                                   "  int a[] = {1, 2, 3};\n"
                                   "  return *std::find_if(a, a + 3, static_cast<bool (*)(const int&)>(nullptr));\n"
                                   "}\n"
                                   "static_assert(nullp() == 0);\n";
    std::string null_find_if_err;
    CHECK_FALSE(syntax_only(c.cxx, null_find_if, true, &null_find_if_err));

    const fs::path inverted_count = c.tmp / "invert_count_const.cpp";
    std::ofstream(inverted_count) << "#include <algorithm>\n"
                                     "constexpr int inverted() {\n"
                                     "  int a[] = {1, 2, 3};\n"
                                     "  return static_cast<int>(std::count(a + 2, a, 1));\n"
                                     "}\n"
                                     "static_assert(inverted() == 0);\n";
    std::string inverted_count_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_count, true, &inverted_count_err));

    const fs::path quant_fn = c.tmp / "quant_fn.cpp";
    std::ofstream(quant_fn) << "#include <algorithm>\n"
                               "bool is_one(const int& x) { return x == 1; }\n"
                               "bool use(int* a, int n) {\n"
                               "  return std::all_of(a, a + n, is_one) && std::any_of(a, a + n, is_one) &&\n"
                               "         std::none_of(a, a + n, is_one);\n"
                               "}\n";
    std::string quant_fn_err;
    CHECK(syntax_only(c.cxx, quant_fn, false, &quant_fn_err));
    CHECK(syntax_only(c.cxx, quant_fn, true, &quant_fn_err));

    const fs::path quant_lambda = c.tmp / "quant_lambda.cpp";
    std::ofstream(quant_lambda) << "#include <algorithm>\n"
                                   "bool use(int* a, int n) {\n"
                                   "  return std::all_of(a, a + n, [](int x) { return x == 0; }) &&\n"
                                   "         std::any_of(a, a + n, [](int x) { return x == 0; }) &&\n"
                                   "         std::none_of(a, a + n, [](int x) { return x == 0; });\n"
                                   "}\n";
    std::string quant_lambda_err;
    CHECK(syntax_only(c.cxx, quant_lambda, false, &quant_lambda_err));
    // Any callable is accepted (one overload, _Pred); only a null pointer traps.
    CHECK(syntax_only(c.cxx, quant_lambda, true, &quant_lambda_err));

    const fs::path quant_val = c.tmp / "quant_val.cpp";
    std::ofstream(quant_val) << "#include <algorithm>\n"
                                "bool is_zero(int x) { return x == 0; }\n"
                                "bool use(int* a, int n) {\n"
                                "  return std::all_of(a, a + n, is_zero) && std::any_of(a, a + n, is_zero) &&\n"
                                "         std::none_of(a, a + n, is_zero);\n"
                                "}\n";
    std::string quant_val_err;
    CHECK(syntax_only(c.cxx, quant_val, false, &quant_val_err));
    // Any callable is accepted (one overload, _Pred); only a null pointer traps.
    CHECK(syntax_only(c.cxx, quant_val, true, &quant_val_err));

    const fs::path quant_empty_ok = c.tmp / "quant_empty_ok.cpp";
    std::ofstream(quant_empty_ok) << "#include <algorithm>\n"
                                     "constexpr bool is_one(const int& x) { return x == 1; }\n"
                                     "constexpr bool checks() {\n"
                                     "  int a[] = {1, 2, 3};\n"
                                     "  return std::all_of(a, a, is_one) && !std::any_of(a, a, is_one) &&\n"
                                     "         std::none_of(a, a, is_one);\n"
                                     "}\n"
                                     "static_assert(checks());\n";
    std::string quant_empty_ok_err;
    CHECK(syntax_only(c.cxx, quant_empty_ok, false, &quant_empty_ok_err));
    CHECK(syntax_only(c.cxx, quant_empty_ok, true, &quant_empty_ok_err));

    const fs::path inverted_all = c.tmp / "invert_all_const.cpp";
    std::ofstream(inverted_all) << "#include <algorithm>\n"
                                   "constexpr bool is_one(const int& x) { return x == 1; }\n"
                                   "constexpr bool inverted() {\n"
                                   "  int a[] = {1, 2, 3};\n"
                                   "  return std::all_of(a + 2, a, is_one);\n"
                                   "}\n"
                                   "static_assert(inverted());\n";
    std::string inverted_all_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_all, true, &inverted_all_err));

    const fs::path inverted_any = c.tmp / "invert_any_const.cpp";
    std::ofstream(inverted_any) << "#include <algorithm>\n"
                                   "constexpr bool is_one(const int& x) { return x == 1; }\n"
                                   "constexpr bool inverted() {\n"
                                   "  int a[] = {1, 2, 3};\n"
                                   "  return std::any_of(a + 2, a, is_one);\n"
                                   "}\n"
                                   "static_assert(!inverted());\n";
    std::string inverted_any_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_any, true, &inverted_any_err));

    const fs::path inverted_none = c.tmp / "invert_none_const.cpp";
    std::ofstream(inverted_none) << "#include <algorithm>\n"
                                    "constexpr bool is_one(const int& x) { return x == 1; }\n"
                                    "constexpr bool inverted() {\n"
                                    "  int a[] = {1, 2, 3};\n"
                                    "  return std::none_of(a + 2, a, is_one);\n"
                                    "}\n"
                                    "static_assert(inverted());\n";
    std::string inverted_none_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_none, true, &inverted_none_err));

    const fs::path null_all = c.tmp / "null_all_const.cpp";
    std::ofstream(null_all) << "#include <algorithm>\n"
                               "constexpr bool nullp() {\n"
                               "  int a[] = {1, 2, 3};\n"
                               "  return std::all_of(a, a + 3, static_cast<bool (*)(const int&)>(nullptr));\n"
                               "}\n"
                               "static_assert(nullp());\n";
    std::string null_all_err;
    CHECK_FALSE(syntax_only(c.cxx, null_all, true, &null_all_err));

    const fs::path null_any = c.tmp / "null_any_const.cpp";
    std::ofstream(null_any) << "#include <algorithm>\n"
                               "constexpr bool nullp() {\n"
                               "  int a[] = {1, 2, 3};\n"
                               "  return std::any_of(a, a + 3, static_cast<bool (*)(const int&)>(nullptr));\n"
                               "}\n"
                               "static_assert(!nullp());\n";
    std::string null_any_err;
    CHECK_FALSE(syntax_only(c.cxx, null_any, true, &null_any_err));

    const fs::path null_none = c.tmp / "null_none_const.cpp";
    std::ofstream(null_none) << "#include <algorithm>\n"
                                "constexpr bool nullp() {\n"
                                "  int a[] = {1, 2, 3};\n"
                                "  return std::none_of(a, a + 3, static_cast<bool (*)(const int&)>(nullptr));\n"
                                "}\n"
                                "static_assert(nullp());\n";
    std::string null_none_err;
    CHECK_FALSE(syntax_only(c.cxx, null_none, true, &null_none_err));

    const fs::path fill_ok = c.tmp / "fill_ok.cpp";
    std::ofstream(fill_ok) << "#include <algorithm>\n"
                                  "void use(int* a, int n, const int& v) {\n"
                                  "  std::fill(a, a + n, v);\n"
                                  "}\n";
    std::string fill_ok_err;
    CHECK(syntax_only(c.cxx, fill_ok, false, &fill_ok_err));
    CHECK(syntax_only(c.cxx, fill_ok, true, &fill_ok_err));

    const fs::path fill_long = c.tmp / "fill_long.cpp";
    std::ofstream(fill_long) << "#include <algorithm>\n"
                                "void use(int* a, int n) {\n"
                                "  std::fill(a, a + n, 0L);\n"
                                "}\n";
    std::string fill_long_err;
    CHECK(syntax_only(c.cxx, fill_long, false, &fill_long_err));
    CHECK_FALSE(syntax_only(c.cxx, fill_long, true, &fill_long_err));

    const fs::path fill_n = c.tmp / "fill_n.cpp";
    std::ofstream(fill_n) << "#include <algorithm>\n"
                             "int* use(int* a, int n) {\n"
                             "  return std::fill_n(a, n, 0);\n"
                             "}\n";
    std::string fill_n_err;
    CHECK(syntax_only(c.cxx, fill_n, false, &fill_n_err));
    CHECK_FALSE(syntax_only(c.cxx, fill_n, true, &fill_n_err));

    const fs::path fill_empty_ok = c.tmp / "fill_empty_ok.cpp";
    std::ofstream(fill_empty_ok) << "#include <algorithm>\n"
                                    "constexpr int empty() {\n"
                                    "  int a[] = {1, 2, 3};\n"
                                    "  std::fill(a, a, 0);\n"
                                    "  return a[0];\n"
                                    "}\n"
                                    "static_assert(empty() == 1);\n"
                                    "constexpr int empty_null() {\n"
                                    "  std::fill(static_cast<int*>(nullptr), static_cast<int*>(nullptr), 0);\n"
                                    "  return 1;\n"
                                    "}\n"
                                    "static_assert(empty_null() == 1);\n";
    std::string fill_empty_ok_err;
    CHECK(syntax_only(c.cxx, fill_empty_ok, false, &fill_empty_ok_err));
    CHECK(syntax_only(c.cxx, fill_empty_ok, true, &fill_empty_ok_err));

    const fs::path inverted_fill = c.tmp / "invert_fill_const.cpp";
    std::ofstream(inverted_fill) << "#include <algorithm>\n"
                                    "constexpr int inverted() {\n"
                                    "  int a[] = {1, 2, 3};\n"
                                    "  std::fill(a + 2, a, 0);\n"
                                    "  return a[0];\n"
                                    "}\n"
                                    "static_assert(inverted() == 1);\n";
    std::string inverted_fill_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_fill, true, &inverted_fill_err));
    CHECK(inverted_fill_err.find("__prism_check") != std::string::npos);

    const fs::path null_fill_const = c.tmp / "null_fill_const.cpp";
    std::ofstream(null_fill_const) << "#include <algorithm>\n"
                                      "constexpr int null_dest() {\n"
                                      "  int a[] = {1, 2, 3};\n"
                                      "  int* first = nullptr;\n"
                                      "  int* last = a + 1;\n"
                                      "  std::fill(first, last, 0);\n"
                                      "  return a[0];\n"
                                      "}\n"
                                      "static_assert(null_dest() == 1);\n";
    std::string null_fill_const_err;
    CHECK_FALSE(syntax_only(c.cxx, null_fill_const, true, &null_fill_const_err));

    const fs::path copy_ok = c.tmp / "copy_ok.cpp";
    std::ofstream(copy_ok) << "#include <algorithm>\n"
                                  "int* use(int* a, int* b, int n) {\n"
                                  "  return std::copy(a, a + n, b);\n"
                                  "}\n";
    std::string copy_ok_err;
    CHECK(syntax_only(c.cxx, copy_ok, false, &copy_ok_err));
    CHECK(syntax_only(c.cxx, copy_ok, true, &copy_ok_err));

    const fs::path copy_const_src = c.tmp / "copy_const_src.cpp";
    std::ofstream(copy_const_src) << "#include <algorithm>\n"
                                     "int* use(const int* a, int* b, int n) {\n"
                                     "  return std::copy(a, a + n, b);\n"
                                     "}\n";
    std::string copy_const_src_err;
    CHECK(syntax_only(c.cxx, copy_const_src, false, &copy_const_src_err));
    CHECK_FALSE(syntax_only(c.cxx, copy_const_src, true, &copy_const_src_err));

    const fs::path copy_n = c.tmp / "copy_n.cpp";
    std::ofstream(copy_n) << "#include <algorithm>\n"
                             "#include <cstddef>\n"
                             "int* use(int* a, int* b, std::size_t n) {\n"
                             "  return std::copy_n(a, n, b);\n"
                             "}\n";
    std::string copy_n_err;
    CHECK(syntax_only(c.cxx, copy_n, false, &copy_n_err));
    CHECK(syntax_only(c.cxx, copy_n, true, &copy_n_err));

    const fs::path copy_n_int = c.tmp / "copy_n_int.cpp";
    std::ofstream(copy_n_int) << "#include <algorithm>\n"
                                 "int* use(int* a, int* b, int n) {\n"
                                 "  return std::copy_n(a, n, b);\n"
                                 "}\n";
    std::string copy_n_int_err;
    CHECK(syntax_only(c.cxx, copy_n_int, false, &copy_n_int_err));
    CHECK(syntax_only(c.cxx, copy_n_int, true, &copy_n_int_err));

    const fs::path copy_n_const_src = c.tmp / "copy_n_const_src.cpp";
    std::ofstream(copy_n_const_src) << "#include <algorithm>\n"
                                       "#include <cstddef>\n"
                                       "int* use(const int* a, int* b, std::size_t n) {\n"
                                       "  return std::copy_n(a, n, b);\n"
                                       "}\n";
    std::string copy_n_const_src_err;
    CHECK(syntax_only(c.cxx, copy_n_const_src, false, &copy_n_const_src_err));
    CHECK_FALSE(syntax_only(c.cxx, copy_n_const_src, true, &copy_n_const_src_err));

    const fs::path copy_backward = c.tmp / "copy_backward.cpp";
    std::ofstream(copy_backward) << "#include <algorithm>\n"
                                    "int* use(int* a, int* b, int n) {\n"
                                    "  return std::copy_backward(a, a + n, b + n);\n"
                                    "}\n";
    std::string copy_backward_err;
    CHECK(syntax_only(c.cxx, copy_backward, false, &copy_backward_err));
    CHECK(syntax_only(c.cxx, copy_backward, true, &copy_backward_err));

    const fs::path copy_backward_const_src = c.tmp / "copy_backward_const_src.cpp";
    std::ofstream(copy_backward_const_src) << "#include <algorithm>\n"
                                              "int* use(const int* a, int* b, int n) {\n"
                                              "  return std::copy_backward(a, a + n, b + n);\n"
                                              "}\n";
    std::string copy_backward_const_src_err;
    CHECK(syntax_only(c.cxx, copy_backward_const_src, false, &copy_backward_const_src_err));
    CHECK_FALSE(syntax_only(c.cxx, copy_backward_const_src, true, &copy_backward_const_src_err));

    const fs::path copy_empty_ok = c.tmp / "copy_empty_ok.cpp";
    std::ofstream(copy_empty_ok) << "#include <algorithm>\n"
                                    "constexpr int empty() {\n"
                                    "  int a[] = {1, 2, 3};\n"
                                    "  int b[] = {8, 7, 6};\n"
                                    "  int* end = std::copy(a, a, b);\n"
                                    "  return (end == b && b[0] == 8 && a[0] == 1) ? 1 : 0;\n"
                                    "}\n"
                                    "static_assert(empty() == 1);\n"
                                    "constexpr int empty_null() {\n"
                                    "  int* end = std::copy(static_cast<int*>(nullptr), static_cast<int*>(nullptr),\n"
                                    "                       static_cast<int*>(nullptr));\n"
                                    "  return end == nullptr ? 1 : 0;\n"
                                    "}\n"
                                    "static_assert(empty_null() == 1);\n";
    std::string copy_empty_ok_err;
    CHECK(syntax_only(c.cxx, copy_empty_ok, false, &copy_empty_ok_err));
    CHECK(syntax_only(c.cxx, copy_empty_ok, true, &copy_empty_ok_err));

    const fs::path inverted_copy = c.tmp / "invert_copy_const.cpp";
    std::ofstream(inverted_copy) << "#include <algorithm>\n"
                                    "constexpr int inverted() {\n"
                                    "  int a[] = {1, 2, 3};\n"
                                    "  int b[] = {0, 0, 0};\n"
                                    "  std::copy(a + 2, a, b);\n"
                                    "  return b[0];\n"
                                    "}\n"
                                    "static_assert(inverted() == 0);\n";
    std::string inverted_copy_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_copy, true, &inverted_copy_err));
    CHECK(inverted_copy_err.find("__prism_check") != std::string::npos);

    const fs::path null_copy_const = c.tmp / "null_copy_const.cpp";
    std::ofstream(null_copy_const) << "#include <algorithm>\n"
                                      "constexpr int null_dest() {\n"
                                      "  int a[] = {1, 2, 3};\n"
                                      "  int* dest = nullptr;\n"
                                      "  std::copy(a, a + 3, dest);\n"
                                      "  return a[0];\n"
                                      "}\n"
                                      "static_assert(null_dest() == 1);\n";
    std::string null_copy_const_err;
    CHECK_FALSE(syntax_only(c.cxx, null_copy_const, true, &null_copy_const_err));
    CHECK(null_copy_const_err.find("__prism_check") != std::string::npos);

    const fs::path overlap_copy_const = c.tmp / "overlap_copy_const.cpp";
    std::ofstream(overlap_copy_const) << "#include <algorithm>\n"
                                         "constexpr int overlap() {\n"
                                         "  int a[] = {1, 2, 3, 4};\n"
                                         "  std::copy(a, a + 3, a + 1);\n"
                                         "  return a[0];\n"
                                         "}\n"
                                         "static_assert(overlap() == 1);\n";
    std::string overlap_copy_const_err;
    CHECK_FALSE(syntax_only(c.cxx, overlap_copy_const, true, &overlap_copy_const_err));
    CHECK(overlap_copy_const_err.find("__prism_check") != std::string::npos);

    const fs::path copy_n_empty_ok = c.tmp / "copy_n_empty_ok.cpp";
    std::ofstream(copy_n_empty_ok) << "#include <algorithm>\n"
                                      "#include <cstddef>\n"
                                      "constexpr int empty() {\n"
                                      "  int a[] = {1, 2, 3};\n"
                                      "  int b[] = {8, 7, 6};\n"
                                      "  int* end = std::copy_n(a, static_cast<std::size_t>(0), b);\n"
                                      "  return (end == b && b[0] == 8 && a[0] == 1) ? 1 : 0;\n"
                                      "}\n"
                                      "static_assert(empty() == 1);\n"
                                      "constexpr int empty_null() {\n"
                                      "  int* end = std::copy_n(static_cast<int*>(nullptr), static_cast<std::size_t>(0),\n"
                                      "                          static_cast<int*>(nullptr));\n"
                                      "  return end == nullptr ? 1 : 0;\n"
                                      "}\n"
                                      "static_assert(empty_null() == 1);\n"
                                      "constexpr int empty_null_src() {\n"
                                      "  int b[] = {8, 7, 6};\n"
                                      "  int* end = std::copy_n(static_cast<int*>(nullptr), static_cast<std::size_t>(0), b);\n"
                                      "  return (end == b && b[0] == 8) ? 1 : 0;\n"
                                      "}\n"
                                      "static_assert(empty_null_src() == 1);\n"
                                      "constexpr int empty_null_dst() {\n"
                                      "  int a[] = {1, 2, 3};\n"
                                      "  int* end = std::copy_n(a, static_cast<std::size_t>(0), static_cast<int*>(nullptr));\n"
                                      "  return (end == nullptr && a[0] == 1) ? 1 : 0;\n"
                                      "}\n"
                                      "static_assert(empty_null_dst() == 1);\n";
    std::string copy_n_empty_ok_err;
    CHECK(syntax_only(c.cxx, copy_n_empty_ok, false, &copy_n_empty_ok_err));
    CHECK(syntax_only(c.cxx, copy_n_empty_ok, true, &copy_n_empty_ok_err));

    const fs::path null_copy_n_src_const = c.tmp / "null_copy_n_src_const.cpp";
    std::ofstream(null_copy_n_src_const) << "#include <algorithm>\n"
                                            "#include <cstddef>\n"
                                            "constexpr int null_source() {\n"
                                            "  int b[] = {0, 0, 0};\n"
                                            "  std::copy_n(static_cast<int*>(nullptr), static_cast<std::size_t>(3), b);\n"
                                            "  return b[0];\n"
                                            "}\n"
                                            "static_assert(null_source() == 0);\n";
    std::string null_copy_n_src_const_err;
    CHECK_FALSE(syntax_only(c.cxx, null_copy_n_src_const, true, &null_copy_n_src_const_err));
    CHECK(null_copy_n_src_const_err.find("__prism_check") != std::string::npos);

    const fs::path null_copy_n_dst_const = c.tmp / "null_copy_n_dst_const.cpp";
    std::ofstream(null_copy_n_dst_const) << "#include <algorithm>\n"
                                            "#include <cstddef>\n"
                                            "constexpr int null_dest() {\n"
                                            "  int a[] = {1, 2, 3};\n"
                                            "  int* dest = nullptr;\n"
                                            "  std::copy_n(a, static_cast<std::size_t>(3), dest);\n"
                                            "  return a[0];\n"
                                            "}\n"
                                            "static_assert(null_dest() == 1);\n";
    std::string null_copy_n_dst_const_err;
    CHECK_FALSE(syntax_only(c.cxx, null_copy_n_dst_const, true, &null_copy_n_dst_const_err));
    CHECK(null_copy_n_dst_const_err.find("__prism_check") != std::string::npos);

    const fs::path copy_bw_empty_ok = c.tmp / "copy_backward_empty_ok.cpp";
    std::ofstream(copy_bw_empty_ok) << "#include <algorithm>\n"
                                       "constexpr int empty() {\n"
                                       "  int a[] = {1, 2, 3};\n"
                                       "  int b[] = {8, 7, 6};\n"
                                       "  int* end = std::copy_backward(a, a, b);\n"
                                       "  return (end == b && b[0] == 8 && a[0] == 1) ? 1 : 0;\n"
                                       "}\n"
                                       "static_assert(empty() == 1);\n"
                                       "constexpr int empty_null() {\n"
                                       "  int* end = std::copy_backward(static_cast<int*>(nullptr),\n"
                                       "                                 static_cast<int*>(nullptr),\n"
                                       "                                 static_cast<int*>(nullptr));\n"
                                       "  return end == nullptr ? 1 : 0;\n"
                                       "}\n"
                                       "static_assert(empty_null() == 1);\n"
                                       "constexpr int empty_null_dst() {\n"
                                       "  int a[] = {1, 2, 3};\n"
                                       "  int* end = std::copy_backward(a, a, static_cast<int*>(nullptr));\n"
                                       "  return (end == nullptr && a[0] == 1) ? 1 : 0;\n"
                                       "}\n"
                                       "static_assert(empty_null_dst() == 1);\n";
    std::string copy_bw_empty_ok_err;
    CHECK(syntax_only(c.cxx, copy_bw_empty_ok, false, &copy_bw_empty_ok_err));
    CHECK(syntax_only(c.cxx, copy_bw_empty_ok, true, &copy_bw_empty_ok_err));

    const fs::path inverted_copy_bw = c.tmp / "invert_copy_backward_const.cpp";
    std::ofstream(inverted_copy_bw) << "#include <algorithm>\n"
                                       "constexpr int inverted() {\n"
                                       "  int a[] = {1, 2, 3};\n"
                                       "  int b[] = {0, 0, 0};\n"
                                       "  std::copy_backward(a + 2, a, b + 3);\n"
                                       "  return b[0];\n"
                                       "}\n"
                                       "static_assert(inverted() == 0);\n";
    std::string inverted_copy_bw_err;
    CHECK_FALSE(syntax_only(c.cxx, inverted_copy_bw, true, &inverted_copy_bw_err));
    CHECK(inverted_copy_bw_err.find("__prism_check") != std::string::npos);

    const fs::path null_copy_bw_const = c.tmp / "null_copy_backward_const.cpp";
    std::ofstream(null_copy_bw_const) << "#include <algorithm>\n"
                                         "constexpr int null_dest() {\n"
                                         "  int a[] = {1, 2, 3};\n"
                                         "  int* dest = nullptr;\n"
                                         "  std::copy_backward(a, a + 3, dest);\n"
                                         "  return a[0];\n"
                                         "}\n"
                                         "static_assert(null_dest() == 1);\n";
    std::string null_copy_bw_const_err;
    CHECK_FALSE(syntax_only(c.cxx, null_copy_bw_const, true, &null_copy_bw_const_err));
    CHECK(null_copy_bw_const_err.find("__prism_check") != std::string::npos);

    // dest == last is the end of (first, last]. [alg.copy] rules it out.
    // A shift right by one (dest == last + 1) is valid and is in the trace.
    const fs::path overlap_copy_bw_const = c.tmp / "overlap_copy_backward_const.cpp";
    std::ofstream(overlap_copy_bw_const) << "#include <algorithm>\n"
                                            "constexpr int overlap() {\n"
                                            "  int a[] = {1, 2, 3, 4};\n"
                                            "  std::copy_backward(a, a + 3, a + 3);\n"
                                            "  return a[0];\n"
                                            "}\n"
                                            "static_assert(overlap() == 1);\n";
    std::string overlap_copy_bw_const_err;
    CHECK_FALSE(syntax_only(c.cxx, overlap_copy_bw_const, true, &overlap_copy_bw_const_err));
    CHECK(overlap_copy_bw_const_err.find("__prism_check") != std::string::npos);

}
