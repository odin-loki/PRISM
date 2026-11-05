// The <ranges> model is a partial stand-in: size, ssize, empty, begin, end,
// data, cbegin, cend, cdata, and distance. std::ranges::count_if is in the
// <algorithm> model, as it is in libstdc++; the trace and the snippets
// here include both headers. <ranges> alone does not declare count_if,
// against libstdc++ or the model. An array size is N. An lvalue
// with a const size() member returns that size(). ssize converts that value
// to ptrdiff_t, and a value that does not fit is a FUNC-CONTRACT check then
// a trap, not a wrapped negative ptrdiff_t. empty of T(&)[N] is true only
// when N == 0, and that body does not load an element. N == 0 is a second
// overload because T(&)[0] is rejected. An lvalue with a const empty()
// member returns that empty(). begin of T(&)[N] is a pointer to the first
// element and does not load it. end is one past the last and does not load
// it. N == 0 returns the same pointer from both. data of T(&)[N] is a
// pointer to the first element by array-to-pointer decay and does not load
// it. N == 0 returns the decayed pointer and does not load it. cbegin,
// cend, and cdata of T(&)[N] with N >= 1 are const T* by that same decay.
// cbegin is the first element and does not load it. cend is one past the
// last and does not load it. cdata is the decayed pointer and does not load
// it. N == 0 does not match that parameter. A second overload constrained
// with sizeof of the array equal to 0 returns the decayed pointer and does
// not load it. distance of T(&)[N] with N >= 1 is the bound converted to
// ptrdiff_t and does not load an element. A bound that does not fit is a
// FUNC-CONTRACT check then a trap, not a wrapped negative ptrdiff_t.
// N == 0 returns 0 and does not load an element. A pointer has no overload,
// so the call fails to compile. This is not a proof that an iterator stays
// in range or that a pointer has a length. This is not PROVED and not CLEAN.
//
// tests/cxx_models/ranges_trace.cpp is compiled twice when clang++ is
// usable: once against libstdc++ and once with this header first.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include "prose_text.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "ranges"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
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

std::size_t count_of(const std::string& s, const std::string& n) {
    std::size_t c = 0;
    std::size_t i = 0;
    while ((i = s.find(n, i)) != std::string::npos) {
        ++c;
        i += n.size();
    }
    return c;
}

struct RangesCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

RangesCtx& ctx() {
    static RangesCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <ranges>\n"
                                  "#ifndef __GLIBCXX__\n#error the model builds on libstdc++ internals\n#endif\n"
                                  "int main() {\n"
                                  "  int a[4] = {};\n"
                                  "  return std::ranges::size(a) == 4 && std::ranges::ssize(a) == 4 ? 0 : 1;\n"
                                  "}\n",
                                  cxxh::asan_flags());
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("ranges_trace.cpp"), "ranges_trace", cxxh::asan_flags(), with_models);
}

bool compile_snippet(const std::string& cxx, const fs::path& tmp, const std::string& name, const std::string& src,
                     bool with_models, std::string* err = nullptr) {
    const fs::path in = tmp / (name + ".cpp");
    std::ofstream(in) << src;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    std::vector<std::string> argv{cxx, "-std=c++23", "-c", "-Wall", "-Wextra", "-Wpedantic", "-Werror"};
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back(in.string());
    argv.push_back("-o");
    argv.push_back((tmp / (name + ".o")).string());
    return run_cmd(argv, nullptr, err);
}

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

}  // namespace

TEST_CASE("cxx ranges model: size, empty, begin, end, data, cbegin, cend, cdata, distance, find, find_if, find_if_not, and count_if; ssize and distance trap when they do not fit") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = prism_test::unwrap_prose(slurp(header));
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("_GLIBCXX_RANGES") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(count_of(src, "extern \"C\" void __prism_check(int, const char*, const char*);") == 1);
    CHECK(src.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    CHECK(src.find("This header is not a proof that a range is sorted or that a pointer has a length.") !=
          std::string::npos);
    CHECK(src.find("This is not a proof that an iterator stays in range.") != std::string::npos);
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(count_of(src, "This is not PROVED and not CLEAN.") == 1);
    CHECK(count_of(src, "PROVED") == 1);
    CHECK(count_of(src, "CLEAN") == 1);
    CHECK(src.find("constexpr std::size_t size(_Tp (&)[_Nm]) noexcept") != std::string::npos);
    CHECK(src.find("return _Nm;") != std::string::npos);
    CHECK(src.find("size(_Tp& __t)") != std::string::npos);
    CHECK(src.find("ssize(_Tp& __t)") != std::string::npos);
    CHECK(src.find("return static_cast<const _Tp&>(__t).size();") != std::string::npos);
    CHECK(src.find("return static_cast<std::ptrdiff_t>(__n);") != std::string::npos);
    CHECK(src.find("_Tp&&") == std::string::npos);
    CHECK(src.find("size(_Tp*") == std::string::npos);
    CHECK(src.find("ssize(_Tp*") == std::string::npos);
    CHECK(src.find("empty(_Tp*") == std::string::npos);
    CHECK(src.find("begin(_Tp*") == std::string::npos);
    CHECK(src.find("end(_Tp*") == std::string::npos);
    CHECK(src.find("data(_Tp*") == std::string::npos);
    CHECK(src.find("cbegin(_Tp*") == std::string::npos);
    CHECK(src.find("cend(_Tp*") == std::string::npos);
    CHECK(src.find("cdata(_Tp*") == std::string::npos);
    CHECK(src.find("distance(_Tp*") == std::string::npos);
    CHECK(src.find("find(_Tp*") == std::string::npos);
    CHECK(src.find("find_if(_Tp*") == std::string::npos);
    CHECK(src.find("find_if_not(_Tp*") == std::string::npos);
    CHECK(src.find("find(_Arr*") == std::string::npos);
    CHECK(src.find("find_if(_Arr*") == std::string::npos);
    CHECK(src.find("find_if_not(_Arr*") == std::string::npos);
    // ranges::count_if is declared by <algorithm>, as in libstdc++, not here.
    CHECK(src.find("count_if") == std::string::npos);
    CHECK(src.find("(_Tp*") == std::string::npos);
    // The access CPOs end where the views::take / views::drop part starts
    // (pointer subranges over contiguous ranges, which use data()).
    const std::string cpos = src.substr(0, src.find("struct _Prism_ptr_elem;"));
    REQUIRE(cpos.size() < src.size());
    // begin, end, data, find, find_if, and find_if_not return _Tp*. cbegin,
    // cend, and cdata return const _Tp*, and that spelling also contains the
    // substring _Tp*.
    CHECK(count_of(cpos, "_Tp*") == 9);
    CHECK(count_of(cpos, "const _Tp*") == 3);
    for (const char* inc : {"memory", "vector", "string", "iterator", "algorithm", "span"}) {
        CAPTURE(inc);
        CHECK(src.find(std::string("#include <") + inc + ">") == std::string::npos);
    }
    for (const char* name : {"sort(", "view_interface"}) {
        CAPTURE(name);
        CHECK(src.find(name) == std::string::npos);
    }
    CHECK(src.find("namespace views {") != std::string::npos);  // take / drop only

    const auto trap = body_of(src, "void _Prism_ssize_does_not_fit()");
    REQUIRE_FALSE(trap.empty());
    const auto check = trap.find("__prism_check(0, \"FUNC-CONTRACT\", \"ranges::ssize size does not fit in ptrdiff_t\")");
    const auto builtin = trap.find("__builtin_trap()");
    REQUIRE(check != std::string::npos);
    REQUIRE(builtin != std::string::npos);
    CHECK(check < builtin);
    CHECK(trap.find("return") == std::string::npos);
    CHECK(count_of(src, "__prism_check(0, \"FUNC-CONTRACT\", \"ranges::ssize size does not fit in ptrdiff_t\")") == 1);

    const auto dist_trap = body_of(src, "void _Prism_ranges_distance_does_not_fit()");
    REQUIRE_FALSE(dist_trap.empty());
    const auto dist_check =
        dist_trap.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::ranges::distance does not fit in ptrdiff_t\")");
    const auto dist_builtin = dist_trap.find("__builtin_trap()");
    REQUIRE(dist_check != std::string::npos);
    REQUIRE(dist_builtin != std::string::npos);
    CHECK(dist_check < dist_builtin);
    CHECK(dist_trap.find("return") == std::string::npos);
    CHECK(count_of(src, "__prism_check(0, \"FUNC-CONTRACT\", \"std::ranges::distance does not fit in ptrdiff_t\")") == 1);

    const auto conv = body_of(src, "constexpr std::ptrdiff_t _Prism_as_ptrdiff(_Sz __n) noexcept");
    REQUIRE_FALSE(conv.empty());
    const auto call = conv.find("_Prism_ssize_does_not_fit()");
    const auto ret = conv.find("return static_cast<std::ptrdiff_t>(__n);");
    REQUIRE(call != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(call < ret);
    CHECK(count_of(conv, "return ") == 1);
    CHECK(conv.find("return -") == std::string::npos);

    const auto arr_empty = body_of(src, "constexpr bool empty(_Tp (&)[_Nm]) noexcept");
    REQUIRE_FALSE(arr_empty.empty());
    CHECK(arr_empty.find("return _Nm == 0;") != std::string::npos);
    CHECK(arr_empty.find('*') == std::string::npos);
    CHECK(arr_empty.find('[') == std::string::npos);
    CHECK(arr_empty.find('.') == std::string::npos);
    CHECK(arr_empty.find("__t") == std::string::npos);

    const auto zero_empty = body_of(src, "constexpr bool empty(_Arr&) noexcept");
    REQUIRE_FALSE(zero_empty.empty());
    CHECK(zero_empty.find("return true;") != std::string::npos);
    CHECK(zero_empty.find('*') == std::string::npos);
    CHECK(zero_empty.find('[') == std::string::npos);
    CHECK(zero_empty.find('.') == std::string::npos);
    CHECK(zero_empty.find("__t") == std::string::npos);
    // empty, begin, end, data, cbegin, cend, cdata, and distance.
    // empty, begin, end, data, cbegin, cend, cdata, distance, find, find_if,
    // and find_if_not. find_if and find_if_not continue with &&, so this
    // text stops before the closing parenthesis.
    CHECK(count_of(src, "requires(sizeof(_Arr) == 0") == 11);

    const auto mem_empty = body_of(src, "empty(_Tp& __t)");
    REQUIRE_FALSE(mem_empty.empty());
    CHECK(mem_empty.find("return static_cast<const _Tp&>(__t).empty();") != std::string::npos);
    CHECK(mem_empty.find('*') == std::string::npos);
    CHECK(mem_empty.find('[') == std::string::npos);
    CHECK(count_of(src, "constexpr bool empty(_Tp (&)[_Nm]) noexcept") == 1);
    CHECK(count_of(src, "constexpr bool empty(_Arr&) noexcept") == 1);
    CHECK(count_of(src, "empty(_Tp& __t)") == 1);
    CHECK(src.find("empty(_Tp*") == std::string::npos);

    const auto arr_begin = body_of(src, "constexpr _Tp* begin(_Tp (&__a)[_Nm]) noexcept");
    REQUIRE_FALSE(arr_begin.empty());
    CHECK(arr_begin.find("return __a;") != std::string::npos);
    CHECK(arr_begin.find('*') == std::string::npos);
    CHECK(arr_begin.find('[') == std::string::npos);
    CHECK(arr_begin.find('.') == std::string::npos);
    CHECK(arr_begin.find('+') == std::string::npos);

    const auto arr_end = body_of(src, "constexpr _Tp* end(_Tp (&__a)[_Nm]) noexcept");
    REQUIRE_FALSE(arr_end.empty());
    CHECK(arr_end.find("return __a + _Nm;") != std::string::npos);
    CHECK(arr_end.find('*') == std::string::npos);
    CHECK(arr_end.find('[') == std::string::npos);
    CHECK(arr_end.find('.') == std::string::npos);

    const auto zero_begin = body_of(src, "constexpr auto begin(_Arr& __a) noexcept");
    REQUIRE_FALSE(zero_begin.empty());
    CHECK(zero_begin.find("return __a;") != std::string::npos);
    CHECK(zero_begin.find('*') == std::string::npos);
    CHECK(zero_begin.find('[') == std::string::npos);
    CHECK(zero_begin.find('.') == std::string::npos);
    CHECK(zero_begin.find('+') == std::string::npos);

    const auto zero_end = body_of(src, "constexpr auto end(_Arr& __a) noexcept");
    REQUIRE_FALSE(zero_end.empty());
    CHECK(zero_end.find("return __a + 0;") != std::string::npos);
    CHECK(zero_end.find('*') == std::string::npos);
    CHECK(zero_end.find('[') == std::string::npos);
    CHECK(zero_end.find('.') == std::string::npos);

    CHECK(count_of(src, "constexpr _Tp* begin(_Tp (&__a)[_Nm]) noexcept") == 1);
    CHECK(count_of(src, "constexpr _Tp* end(_Tp (&__a)[_Nm]) noexcept") == 1);
    CHECK(count_of(src, "constexpr auto begin(_Arr& __a) noexcept") == 1);
    CHECK(count_of(src, "constexpr auto end(_Arr& __a) noexcept") == 1);

    const auto arr_data = body_of(src, "constexpr _Tp* data(_Tp (&__a)[_Nm]) noexcept");
    REQUIRE_FALSE(arr_data.empty());
    CHECK(arr_data.find("return __a;") != std::string::npos);
    CHECK(arr_data.find('*') == std::string::npos);
    CHECK(arr_data.find('[') == std::string::npos);
    CHECK(arr_data.find('.') == std::string::npos);
    CHECK(arr_data.find('+') == std::string::npos);

    const auto zero_data = body_of(src, "constexpr auto data(_Arr& __a) noexcept");
    REQUIRE_FALSE(zero_data.empty());
    CHECK(zero_data.find("return __a;") != std::string::npos);
    CHECK(zero_data.find('*') == std::string::npos);
    CHECK(zero_data.find('[') == std::string::npos);
    CHECK(zero_data.find('.') == std::string::npos);
    CHECK(zero_data.find('+') == std::string::npos);

    CHECK(count_of(src, "constexpr _Tp* data(_Tp (&__a)[_Nm]) noexcept") == 1);
    CHECK(count_of(src, "constexpr auto data(_Arr& __a) noexcept") == 1);
    CHECK(src.substr(0, src.find("struct _Prism_ptr_elem;")).find(".data()") == std::string::npos);

    const auto arr_cbegin = body_of(src, "constexpr const _Tp* cbegin(_Tp (&__a)[_Nm]) noexcept");
    REQUIRE_FALSE(arr_cbegin.empty());
    CHECK(arr_cbegin.find("return __a;") != std::string::npos);
    CHECK(arr_cbegin.find('*') == std::string::npos);
    CHECK(arr_cbegin.find('[') == std::string::npos);
    CHECK(arr_cbegin.find('.') == std::string::npos);
    CHECK(arr_cbegin.find('+') == std::string::npos);

    const auto arr_cend = body_of(src, "constexpr const _Tp* cend(_Tp (&__a)[_Nm]) noexcept");
    REQUIRE_FALSE(arr_cend.empty());
    CHECK(arr_cend.find("return __a + _Nm;") != std::string::npos);
    CHECK(arr_cend.find('*') == std::string::npos);
    CHECK(arr_cend.find('[') == std::string::npos);
    CHECK(arr_cend.find('.') == std::string::npos);

    const auto zero_cbegin = body_of(src, "constexpr auto cbegin(_Arr& __a) noexcept");
    REQUIRE_FALSE(zero_cbegin.empty());
    CHECK(zero_cbegin.find("return __a;") != std::string::npos);
    CHECK(zero_cbegin.find('*') == std::string::npos);
    CHECK(zero_cbegin.find('[') == std::string::npos);
    CHECK(zero_cbegin.find('.') == std::string::npos);
    CHECK(zero_cbegin.find('+') == std::string::npos);

    const auto zero_cend = body_of(src, "constexpr auto cend(_Arr& __a) noexcept");
    REQUIRE_FALSE(zero_cend.empty());
    CHECK(zero_cend.find("return __a;") != std::string::npos);
    CHECK(zero_cend.find('*') == std::string::npos);
    CHECK(zero_cend.find('[') == std::string::npos);
    CHECK(zero_cend.find('.') == std::string::npos);
    CHECK(zero_cend.find('+') == std::string::npos);

    const auto arr_cdata = body_of(src, "constexpr const _Tp* cdata(_Tp (&__a)[_Nm]) noexcept");
    REQUIRE_FALSE(arr_cdata.empty());
    CHECK(arr_cdata.find("return __a;") != std::string::npos);
    CHECK(arr_cdata.find('*') == std::string::npos);
    CHECK(arr_cdata.find('[') == std::string::npos);
    CHECK(arr_cdata.find('.') == std::string::npos);
    CHECK(arr_cdata.find('+') == std::string::npos);

    const auto zero_cdata = body_of(src, "constexpr auto cdata(_Arr& __a) noexcept");
    REQUIRE_FALSE(zero_cdata.empty());
    CHECK(zero_cdata.find("return __a;") != std::string::npos);
    CHECK(zero_cdata.find('*') == std::string::npos);
    CHECK(zero_cdata.find('[') == std::string::npos);
    CHECK(zero_cdata.find('.') == std::string::npos);
    CHECK(zero_cdata.find('+') == std::string::npos);

    CHECK(count_of(src, "constexpr const _Tp* cbegin(_Tp (&__a)[_Nm]) noexcept") == 1);
    CHECK(count_of(src, "constexpr const _Tp* cend(_Tp (&__a)[_Nm]) noexcept") == 1);
    CHECK(count_of(src, "constexpr const _Tp* cdata(_Tp (&__a)[_Nm]) noexcept") == 1);
    CHECK(count_of(src, "constexpr auto cbegin(_Arr& __a) noexcept") == 1);
    CHECK(count_of(src, "constexpr auto cend(_Arr& __a) noexcept") == 1);
    CHECK(count_of(src, "constexpr auto cdata(_Arr& __a) noexcept") == 1);
    CHECK(count_of(src, "decltype(static_cast<const _Arr&>(__a) + 0)") == 3);
    CHECK(src.find(".cbegin()") == std::string::npos);
    CHECK(src.find(".cend()") == std::string::npos);
    CHECK(src.find(".cdata()") == std::string::npos);
    // begin and data (array and zero-length), cbegin and cdata (array and
    // zero-length), zero-length cend, and the zero-length find, find_if, and
    // find_if_not. end's zero-length body is still + 0.
    CHECK(count_of(src, "return __a;") == 12);
    CHECK(count_of(src, "return __a + _Nm;") == 5);
    CHECK(count_of(src, "return __a + __i;") == 3);
    CHECK(count_of(src, "return __a + 0;") == 1);

    const auto arr_dist = body_of(src, "constexpr std::ptrdiff_t distance(_Tp (&)[_Nm]) noexcept");
    REQUIRE_FALSE(arr_dist.empty());
    const auto dist_call = arr_dist.find("_Prism_ranges_distance_does_not_fit()");
    const auto dist_ret = arr_dist.find("return static_cast<std::ptrdiff_t>(_Nm);");
    REQUIRE(dist_call != std::string::npos);
    REQUIRE(dist_ret != std::string::npos);
    CHECK(dist_call < dist_ret);
    CHECK(count_of(arr_dist, "return ") == 1);
    CHECK(arr_dist.find("return -") == std::string::npos);
    CHECK(arr_dist.find('*') == std::string::npos);
    CHECK(arr_dist.find('[') == std::string::npos);
    CHECK(arr_dist.find('.') == std::string::npos);
    CHECK(arr_dist.find('+') == std::string::npos);
    CHECK(arr_dist.find("__a") == std::string::npos);
    CHECK(arr_dist.find("__t") == std::string::npos);

    const auto zero_dist = body_of(src, "constexpr std::ptrdiff_t distance(_Arr&) noexcept");
    REQUIRE_FALSE(zero_dist.empty());
    CHECK(zero_dist.find("return 0;") != std::string::npos);
    CHECK(count_of(zero_dist, "return ") == 1);
    CHECK(zero_dist.find('*') == std::string::npos);
    CHECK(zero_dist.find('[') == std::string::npos);
    CHECK(zero_dist.find('.') == std::string::npos);
    CHECK(zero_dist.find('+') == std::string::npos);
    CHECK(zero_dist.find("__a") == std::string::npos);
    CHECK(zero_dist.find("__t") == std::string::npos);
    CHECK(count_of(src, "constexpr std::ptrdiff_t distance(_Tp (&)[_Nm]) noexcept") == 1);
    CHECK(count_of(src, "constexpr std::ptrdiff_t distance(_Arr&) noexcept") == 1);
    CHECK(src.find(".distance()") == std::string::npos);

    const auto arr_find =
        body_of(src, "constexpr _Tp* find(_Tp (&__a)[_Nm], const _Prism_type_identity_t<_Tp>& __value)");
    REQUIRE_FALSE(arr_find.empty());
    CHECK(arr_find.find("__i < _Nm") != std::string::npos);
    CHECK(arr_find.find("__a[__i] == __value") != std::string::npos);
    CHECK(arr_find.find("return __a + __i;") != std::string::npos);
    CHECK(arr_find.find("return __a + _Nm;") != std::string::npos);
    CHECK(arr_find.find("[_Nm]") == std::string::npos);
    CHECK(arr_find.find("__i <= _Nm") == std::string::npos);
    CHECK(arr_find.find('*') == std::string::npos);
    CHECK(arr_find.find('.') == std::string::npos);
    CHECK(count_of(arr_find, "return ") == 2);

    const auto zero_find = body_of(src, "constexpr auto find(_Arr& __a, const _Up&) noexcept");
    REQUIRE_FALSE(zero_find.empty());
    CHECK(zero_find.find("return __a;") != std::string::npos);
    CHECK(count_of(zero_find, "return ") == 1);
    CHECK(zero_find.find('*') == std::string::npos);
    CHECK(zero_find.find('[') == std::string::npos);
    CHECK(zero_find.find('.') == std::string::npos);
    CHECK(zero_find.find('+') == std::string::npos);
    CHECK(zero_find.find("__value") == std::string::npos);
    CHECK(zero_find.find("_Up") == std::string::npos);
    CHECK(count_of(src, "constexpr _Tp* find(_Tp (&__a)[_Nm], const _Prism_type_identity_t<_Tp>& __value)") == 1);
    CHECK(count_of(src, "constexpr auto find(_Arr& __a, const _Up&) noexcept") == 1);

    const auto arr_find_if =
        body_of(src, "constexpr _Tp* find_if(_Tp (&__a)[_Nm], bool (*__pred)(const _Prism_type_identity_t<_Tp>&))");
    REQUIRE_FALSE(arr_find_if.empty());
    const auto fi_null = arr_find_if.find("if (__pred == nullptr)");
    const auto fi_check =
        arr_find_if.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::ranges::find_if null predicate\")");
    const auto fi_trap = arr_find_if.find("__builtin_trap()");
    const auto fi_scan = arr_find_if.find("for (std::size_t __i = 0; __i < _Nm; ++__i)");
    const auto fi_call = arr_find_if.find("__pred(__a[__i])");
    const auto fi_hit = arr_find_if.find("return __a + __i;");
    const auto fi_end = arr_find_if.find("return __a + _Nm;");
    REQUIRE(fi_null != std::string::npos);
    REQUIRE(fi_check != std::string::npos);
    REQUIRE(fi_trap != std::string::npos);
    REQUIRE(fi_scan != std::string::npos);
    REQUIRE(fi_call != std::string::npos);
    REQUIRE(fi_hit != std::string::npos);
    REQUIRE(fi_end != std::string::npos);
    CHECK(fi_null < fi_check);
    CHECK(fi_check < fi_trap);
    CHECK(fi_trap < fi_scan);
    CHECK(fi_scan < fi_call);
    CHECK(fi_call < fi_hit);
    CHECK(fi_hit < fi_end);
    const std::string fi_pre = arr_find_if.substr(0, fi_scan);
    CHECK(fi_pre.find("__pred(") == std::string::npos);
    CHECK(fi_pre.find("__a[") == std::string::npos);
    CHECK(fi_pre.find('*') == std::string::npos);
    CHECK(fi_pre.find("return") == std::string::npos);
    CHECK(arr_find_if.find("[_Nm]") == std::string::npos);
    CHECK(arr_find_if.find("__i <= _Nm") == std::string::npos);
    CHECK(arr_find_if.find('*') == std::string::npos);
    CHECK(arr_find_if.find('.') == std::string::npos);
    CHECK(count_of(arr_find_if, "return ") == 2);
    CHECK(count_of(src, "__prism_check(0, \"FUNC-CONTRACT\", \"std::ranges::find_if null predicate\")") == 1);

    const auto zero_find_if = body_of(src, "constexpr auto find_if(_Arr& __a, bool (*)(const _Elem&)) noexcept");
    REQUIRE_FALSE(zero_find_if.empty());
    CHECK(zero_find_if.find("return __a;") != std::string::npos);
    CHECK(count_of(zero_find_if, "return ") == 1);
    CHECK(zero_find_if.find('*') == std::string::npos);
    CHECK(zero_find_if.find('[') == std::string::npos);
    CHECK(zero_find_if.find('.') == std::string::npos);
    CHECK(zero_find_if.find('+') == std::string::npos);
    CHECK(zero_find_if.find("__pred") == std::string::npos);
    CHECK(zero_find_if.find("_Elem") == std::string::npos);
    CHECK(zero_find_if.find("nullptr") == std::string::npos);
    CHECK(count_of(src, "constexpr _Tp* find_if(_Tp (&__a)[_Nm], bool (*__pred)(const _Prism_type_identity_t<_Tp>&))") ==
          1);
    CHECK(count_of(src, "constexpr auto find_if(_Arr& __a, bool (*)(const _Elem&)) noexcept") == 1);

    const auto arr_find_if_not = body_of(
        src, "constexpr _Tp* find_if_not(_Tp (&__a)[_Nm], bool (*__pred)(const _Prism_type_identity_t<_Tp>&))");
    REQUIRE_FALSE(arr_find_if_not.empty());
    const auto fn_null = arr_find_if_not.find("if (__pred == nullptr)");
    const auto fn_check =
        arr_find_if_not.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::ranges::find_if_not null predicate\")");
    const auto fn_trap = arr_find_if_not.find("__builtin_trap()");
    const auto fn_scan = arr_find_if_not.find("for (std::size_t __i = 0; __i < _Nm; ++__i)");
    const auto fn_call = arr_find_if_not.find("!__pred(__a[__i])");
    const auto fn_hit = arr_find_if_not.find("return __a + __i;");
    const auto fn_end = arr_find_if_not.find("return __a + _Nm;");
    REQUIRE(fn_null != std::string::npos);
    REQUIRE(fn_check != std::string::npos);
    REQUIRE(fn_trap != std::string::npos);
    REQUIRE(fn_scan != std::string::npos);
    REQUIRE(fn_call != std::string::npos);
    REQUIRE(fn_hit != std::string::npos);
    REQUIRE(fn_end != std::string::npos);
    CHECK(fn_null < fn_check);
    CHECK(fn_check < fn_trap);
    CHECK(fn_trap < fn_scan);
    CHECK(fn_scan < fn_call);
    CHECK(fn_call < fn_hit);
    CHECK(fn_hit < fn_end);
    const std::string fn_pre = arr_find_if_not.substr(0, fn_scan);
    CHECK(fn_pre.find("__pred(") == std::string::npos);
    CHECK(fn_pre.find("__a[") == std::string::npos);
    CHECK(fn_pre.find('*') == std::string::npos);
    CHECK(fn_pre.find("return") == std::string::npos);
    CHECK(arr_find_if_not.find("[_Nm]") == std::string::npos);
    CHECK(arr_find_if_not.find("__i <= _Nm") == std::string::npos);
    CHECK(arr_find_if_not.find('*') == std::string::npos);
    CHECK(arr_find_if_not.find('.') == std::string::npos);
    CHECK(count_of(arr_find_if_not, "return ") == 2);
    CHECK(count_of(src, "__prism_check(0, \"FUNC-CONTRACT\", \"std::ranges::find_if_not null predicate\")") == 1);

    const auto zero_find_if_not =
        body_of(src, "constexpr auto find_if_not(_Arr& __a, bool (*)(const _Elem&)) noexcept");
    REQUIRE_FALSE(zero_find_if_not.empty());
    CHECK(zero_find_if_not.find("return __a;") != std::string::npos);
    CHECK(count_of(zero_find_if_not, "return ") == 1);
    CHECK(zero_find_if_not.find('*') == std::string::npos);
    CHECK(zero_find_if_not.find('[') == std::string::npos);
    CHECK(zero_find_if_not.find('.') == std::string::npos);
    CHECK(zero_find_if_not.find('+') == std::string::npos);
    CHECK(zero_find_if_not.find("__pred") == std::string::npos);
    CHECK(zero_find_if_not.find("_Elem") == std::string::npos);
    CHECK(zero_find_if_not.find("nullptr") == std::string::npos);
    CHECK(count_of(
              src, "constexpr _Tp* find_if_not(_Tp (&__a)[_Nm], bool (*__pred)(const _Prism_type_identity_t<_Tp>&))") ==
          1);
    CHECK(count_of(src, "constexpr auto find_if_not(_Arr& __a, bool (*)(const _Elem&)) noexcept") == 1);
}

TEST_CASE("cxx ranges model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxranges-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx ranges model: array size, empty, begin, end, data, cbegin, cend, cdata, distance, find, find_if, find_if_not, and count_if match libstdc++, a pointer does not compile") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "ranges_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(header_path().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(has_line(b, "arr 4"));
    CHECK(has_line(b, "sarr 4"));
    CHECK(has_line(b, "box 7"));
    CHECK(has_line(b, "sbox 7"));
    CHECK(has_line(b, "cbox 3"));
    CHECK(has_line(b, "scbox 3"));
    CHECK(has_line(b, "earr 0"));
    CHECK(has_line(b, "cearr 0"));
    CHECK(has_line(b, "inner 0"));
    CHECK(has_line(b, "eflag 1"));
    CHECK(has_line(b, "enflag 0"));
    CHECK(has_line(b, "ecflag 1"));
    CHECK(has_line(b, "eprefer 1"));
    CHECK(has_line(b, "eptrs 0"));
    CHECK(has_line(b, "bdist 4"));
    CHECK(has_line(b, "bfirst 9"));
    CHECK(has_line(b, "blast 6"));
    CHECK(has_line(b, "cbdist 4"));
    CHECK(has_line(b, "cbfirst 2"));
    CHECK(has_line(b, "cblast 5"));
    CHECK(has_line(b, "bpdist 2"));
    CHECK(has_line(b, "bpsame 1"));
    CHECK(has_line(b, "isame 1"));
    CHECK(has_line(b, "dfirst 9"));
    CHECK(has_line(b, "dsame 1"));
    CHECK(has_line(b, "cdfirst 2"));
    CHECK(has_line(b, "cdsame 1"));
    CHECK(has_line(b, "dpsame 1"));
    CHECK(has_line(b, "idsame 1"));
    CHECK(has_line(b, "kbdist 4"));
    CHECK(has_line(b, "kbfirst 9"));
    CHECK(has_line(b, "kblast 6"));
    CHECK(has_line(b, "kbsame 1"));
    CHECK(has_line(b, "kcbdist 4"));
    CHECK(has_line(b, "kcbfirst 2"));
    CHECK(has_line(b, "kcblast 5"));
    CHECK(has_line(b, "kbpdist 2"));
    CHECK(has_line(b, "kbpsame 1"));
    CHECK(has_line(b, "kisame 1"));
    CHECK(has_line(b, "kdfirst 9"));
    CHECK(has_line(b, "kdsame 1"));
    CHECK(has_line(b, "kcdfirst 2"));
    CHECK(has_line(b, "kcdsame 1"));
    CHECK(has_line(b, "kdpsame 1"));
    CHECK(has_line(b, "kidsame 1"));
    CHECK(has_line(b, "dist 4"));
    CHECK(has_line(b, "cdist 4"));
    CHECK(has_line(b, "idist 4"));
    CHECK(has_line(b, "pdist 2"));
    CHECK(has_line(b, "ffirst 9"));
    CHECK(has_line(b, "fnext 1"));
    CHECK(has_line(b, "flast 6"));
    CHECK(has_line(b, "fmiss 1"));
    CHECK(has_line(b, "fcfirst 2"));
    CHECK(has_line(b, "fclast 5"));
    CHECK(has_line(b, "fcmiss 1"));
    CHECK(has_line(b, "fpfind 1"));
    CHECK(has_line(b, "fpmiss 1"));
    CHECK(has_line(b, "ifhit 1 2 3"));
    CHECK(has_line(b, "iffirst 1 2"));
    CHECK(has_line(b, "iffront 0 1"));
    CHECK(has_line(b, "ifmiss 1 4"));
    CHECK(has_line(b, "iftie 1 1"));
    CHECK(has_line(b, "ifone 1"));
    CHECK(has_line(b, "ifkeep 1"));
    CHECK(has_line(b, "ifcfirst 2"));
    CHECK(has_line(b, "ifclast 5"));
    CHECK(has_line(b, "ifcmiss 1"));
    CHECK(has_line(b, "ifpfind 1"));
    CHECK(has_line(b, "ifpmiss 1"));
    CHECK(has_line(b, "nfhit 1 0 1"));
    CHECK(has_line(b, "nfnext 1 2"));
    CHECK(has_line(b, "nflast 3 4"));
    CHECK(has_line(b, "nfmiss 1 4"));
    CHECK(has_line(b, "nftie 1 1"));
    CHECK(has_line(b, "nftieall 1 2"));
    CHECK(has_line(b, "nfone 1"));
    CHECK(has_line(b, "nfkeep 1"));
    CHECK(has_line(b, "nfcfirst 2"));
    CHECK(has_line(b, "nfcnext 1"));
    CHECK(has_line(b, "nfcmiss 1"));
    CHECK(has_line(b, "nfpfind 1"));
    CHECK(has_line(b, "nfpmiss 1"));
    CHECK(has_line(b, "cfhit 1 4"));
    CHECK(has_line(b, "cftwo 2 4"));
    CHECK(has_line(b, "cffront 1 4"));
    CHECK(has_line(b, "cfmiss 0 4"));
    CHECK(has_line(b, "cftie 2 2"));
    CHECK(has_line(b, "cftiemiss 0 2"));
    CHECK(has_line(b, "cfone 1"));
    CHECK(has_line(b, "cfkeep 1"));
    CHECK(has_line(b, "cfcfirst 1"));
    CHECK(has_line(b, "cfczero 2 4"));
    CHECK(has_line(b, "cfcall 4 4"));
    CHECK(has_line(b, "cfcmiss 0"));
    CHECK(has_line(b, "cfpnull 2"));
    CHECK(has_line(b, "cfpmiss 0"));

    std::string out, err;
    CHECK_FALSE(run_cmd({mod.string(), "huge"}, &out, &err));
    const std::string both = out + err;
    CHECK(both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(both.find("ranges::ssize size does not fit in ptrdiff_t") != std::string::npos);
    CHECK(out.find("wrapped=") == std::string::npos);

    std::string null_out, null_err;
    CHECK_FALSE(run_cmd({mod.string(), "null"}, &null_out, &null_err));
    const std::string null_both = null_out + null_err;
    CHECK(null_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(null_both.find("std::ranges::find_if null predicate") != std::string::npos);
    CHECK(null_out.find("called") == std::string::npos);

    std::string notnull_out, notnull_err;
    CHECK_FALSE(run_cmd({mod.string(), "notnull"}, &notnull_out, &notnull_err));
    const std::string notnull_both = notnull_out + notnull_err;
    CHECK(notnull_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(notnull_both.find("std::ranges::find_if_not null predicate") != std::string::npos);
    CHECK(notnull_out.find("called") == std::string::npos);

    std::string countnull_out, countnull_err;
    CHECK_FALSE(run_cmd({mod.string(), "countnull"}, &countnull_out, &countnull_err));
    const std::string countnull_both = countnull_out + countnull_err;
    CHECK(countnull_both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(countnull_both.find("std::ranges::count_if null predicate") != std::string::npos);
    CHECK(countnull_out.find("called") == std::string::npos);

    const char* ok_src =
        "#include <ranges>\n"
        "struct Box { int n; constexpr int size() const { return n; } };\n"
        "struct Flag { bool e; constexpr bool empty() const { return e; } };\n"
        "struct Prefer {\n"
        "  constexpr bool empty() const { return true; }\n"
        "  constexpr int size() const { return 4; }\n"
        "};\n"
        "int main() {\n"
        "  int a[4] = {};\n"
        "  const int ca[4] = {};\n"
        "  Box b; b.n = 7;\n"
        "  const Box k{3};\n"
        "  Flag t; t.e = true;\n"
        "  Flag u; u.e = false;\n"
        "  const Flag cf{true};\n"
        "  Prefer pr;\n"
        "  if (std::ranges::size(a) != 4 || std::ranges::ssize(a) != 4) return 1;\n"
        "  if (std::ranges::size(b) != 7 || std::ranges::ssize(b) != 7) return 1;\n"
        "  if (std::ranges::size(k) != 3 || std::ranges::ssize(k) != 3) return 1;\n"
        "  if (std::ranges::empty(a) || std::ranges::empty(ca)) return 1;\n"
        "  if (!std::ranges::empty(t) || std::ranges::empty(u) || !std::ranges::empty(cf)) return 1;\n"
        "  if (!std::ranges::empty(pr) || std::ranges::size(pr) != 4) return 1;\n"
        "  return 0;\n"
        "}\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_ok", ok_src, true));

    const char* ptr_src =
        "#include <ranges>\n"
        "#include <cstddef>\n"
        "std::size_t f(int* p) { return std::ranges::size(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_ptr", ptr_src, true));

    // A pointer to an array is not an array. size must not give it a length.
    const char* pbound_src =
        "#include <ranges>\n"
        "#include <cstddef>\n"
        "std::size_t f(int (*p)[4]) { return std::ranges::size(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_pbound", pbound_src, true));

    const char* rval_src =
        "#include <ranges>\n"
        "struct Box { int size() const { return 1; } };\n"
        "int f() { return static_cast<int>(std::ranges::size(Box{})); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_rval", rval_src, true));

    const char* mut_src =
        "#include <ranges>\n"
        "struct Mut { int size() { return 1; } };\n"
        "int f() { Mut m; return static_cast<int>(std::ranges::size(m)); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_mut", mut_src, true));

    const char* begin_src =
        "#include <ranges>\n"
        "int* f(int (&a)[4]) { return std::ranges::begin(a); }\n"
        "int* g(int (&a)[4]) { return std::ranges::end(a); }\n"
        "const int* h(const int (&a)[4]) { return std::ranges::begin(a); }\n"
        "const int* i(const int (&a)[4]) { return std::ranges::end(a); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_begin", begin_src, true));

    const char* begin_ptr_src =
        "#include <ranges>\n"
        "int* f(int* p) { return std::ranges::begin(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_begin_ptr", begin_ptr_src, true));

    const char* end_ptr_src =
        "#include <ranges>\n"
        "int* f(int* p) { return std::ranges::end(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_end_ptr", end_ptr_src, true));

    const char* begin_pbound_src =
        "#include <ranges>\n"
        "int* f(int (*p)[4]) { return std::ranges::begin(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_begin_pbound", begin_pbound_src, true));

    const char* end_pbound_src =
        "#include <ranges>\n"
        "int* f(int (*p)[4]) { return std::ranges::end(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_end_pbound", end_pbound_src, true));

    const char* begin_mem_src =
        "#include <ranges>\n"
        "struct Has { int* begin() const; int* end() const; };\n"
        "int* f(Has& h) { return std::ranges::begin(h); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_begin_mem", begin_mem_src, true));

    const char* data_src =
        "#include <ranges>\n"
        "int* f(int (&a)[4]) { return std::ranges::data(a); }\n"
        "const int* h(const int (&a)[4]) { return std::ranges::data(a); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_data", data_src, true));

    const char* data_ptr_src =
        "#include <ranges>\n"
        "int* f(int* p) { return std::ranges::data(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_data_ptr", data_ptr_src, true));

    const char* data_pbound_src =
        "#include <ranges>\n"
        "int* f(int (*p)[4]) { return std::ranges::data(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_data_pbound", data_pbound_src, true));

    const char* data_mem_src =
        "#include <ranges>\n"
        "struct Has { int* data() const; };\n"
        "int* f(Has& h) { return std::ranges::data(h); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_data_mem", data_mem_src, true));

    const char* cbegin_src =
        "#include <ranges>\n"
        "#include <type_traits>\n"
        "static_assert(std::is_same_v<decltype(std::ranges::cbegin(std::declval<int (&)[4]>())), const int*>);\n"
        "static_assert(std::is_same_v<decltype(std::ranges::cend(std::declval<int (&)[4]>())), const int*>);\n"
        "static_assert(std::is_same_v<decltype(std::ranges::cdata(std::declval<int (&)[4]>())), const int*>);\n"
        "static_assert(std::is_same_v<decltype(std::ranges::cbegin(std::declval<const int (&)[4]>())), const int*>);\n"
        "static_assert(std::is_same_v<decltype(std::ranges::cend(std::declval<const int (&)[4]>())), const int*>);\n"
        "static_assert(std::is_same_v<decltype(std::ranges::cdata(std::declval<const int (&)[4]>())), const int*>);\n"
        "const int* f(int (&a)[4]) { return std::ranges::cbegin(a); }\n"
        "const int* g(int (&a)[4]) { return std::ranges::cend(a); }\n"
        "const int* h(const int (&a)[4]) { return std::ranges::cbegin(a); }\n"
        "const int* i(const int (&a)[4]) { return std::ranges::cend(a); }\n"
        "const int* j(int (&a)[4]) { return std::ranges::cdata(a); }\n"
        "const int* k(const int (&a)[4]) { return std::ranges::cdata(a); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_cbegin", cbegin_src, true));

    const char* cbegin_mut_src =
        "#include <ranges>\n"
        "int* f(int (&a)[4]) { return std::ranges::cbegin(a); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cbegin_mut", cbegin_mut_src, true));

    const char* cend_mut_src =
        "#include <ranges>\n"
        "int* f(int (&a)[4]) { return std::ranges::cend(a); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cend_mut", cend_mut_src, true));

    const char* cdata_mut_src =
        "#include <ranges>\n"
        "int* f(int (&a)[4]) { return std::ranges::cdata(a); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cdata_mut", cdata_mut_src, true));

    const char* cbegin_ptr_src =
        "#include <ranges>\n"
        "const int* f(int* p) { return std::ranges::cbegin(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cbegin_ptr", cbegin_ptr_src, true));

    const char* cend_ptr_src =
        "#include <ranges>\n"
        "const int* f(int* p) { return std::ranges::cend(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cend_ptr", cend_ptr_src, true));

    const char* cdata_ptr_src =
        "#include <ranges>\n"
        "const int* f(int* p) { return std::ranges::cdata(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cdata_ptr", cdata_ptr_src, true));

    const char* cbegin_pbound_src =
        "#include <ranges>\n"
        "const int* f(int (*p)[4]) { return std::ranges::cbegin(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cbegin_pbound", cbegin_pbound_src, true));

    const char* cend_pbound_src =
        "#include <ranges>\n"
        "const int* f(int (*p)[4]) { return std::ranges::cend(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cend_pbound", cend_pbound_src, true));

    const char* cdata_pbound_src =
        "#include <ranges>\n"
        "const int* f(int (*p)[4]) { return std::ranges::cdata(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cdata_pbound", cdata_pbound_src, true));

    const char* cbegin_mem_src =
        "#include <ranges>\n"
        "struct Has { const int* begin() const; const int* end() const; };\n"
        "const int* f(Has& h) { return std::ranges::cbegin(h); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cbegin_mem", cbegin_mem_src, true));

    const char* cend_mem_src =
        "#include <ranges>\n"
        "struct Has { const int* begin() const; const int* end() const; };\n"
        "const int* f(Has& h) { return std::ranges::cend(h); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cend_mem", cend_mem_src, true));

    const char* cdata_mem_src =
        "#include <ranges>\n"
        "struct Has { const int* data() const; };\n"
        "const int* f(Has& h) { return std::ranges::cdata(h); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_cdata_mem", cdata_mem_src, true));

    const char* dist_src =
        "#include <ranges>\n"
        "#include <type_traits>\n"
        "static_assert(std::is_same_v<decltype(std::ranges::distance(std::declval<int (&)[4]>())), std::ptrdiff_t>);\n"
        "static_assert(std::is_same_v<decltype(std::ranges::distance(std::declval<const int (&)[4]>())), "
        "std::ptrdiff_t>);\n"
        "constexpr int ak[4] = {1, 2, 3, 4};\n"
        "static_assert(std::ranges::distance(ak) == 4);\n"
        "std::ptrdiff_t f(int (&a)[4]) { return std::ranges::distance(a); }\n"
        "std::ptrdiff_t g(const int (&a)[4]) { return std::ranges::distance(a); }\n"
        "std::ptrdiff_t h(int* (&a)[2]) { return std::ranges::distance(a); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_dist", dist_src, true));

    const char* dist_ptr_src =
        "#include <ranges>\n"
        "std::ptrdiff_t f(int* p) { return std::ranges::distance(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_dist_ptr", dist_ptr_src, true));

    const char* dist_pbound_src =
        "#include <ranges>\n"
        "std::ptrdiff_t f(int (*p)[4]) { return std::ranges::distance(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_dist_pbound", dist_pbound_src, true));

    const char* dist_pair_src =
        "#include <ranges>\n"
        "std::ptrdiff_t f(int* b, int* e) { return std::ranges::distance(b, e); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_dist_pair", dist_pair_src, true));

    const char* dist_mem_src =
        "#include <ranges>\n"
        "struct Has { int size() const { return 4; } };\n"
        "std::ptrdiff_t f(Has& h) { return std::ranges::distance(h); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_dist_mem", dist_mem_src, true));

    const char* sort_src =
        "#include <ranges>\n"
        "void f(int* b, int* e) { std::ranges::sort(b, e); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_sort", sort_src, true));

    const char* find_src =
        "#include <ranges>\n"
        "int* f(int* b, int* e, int v) { return std::ranges::find(b, e, v); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find", find_src, true));

    const char* find_arr_src =
        "#include <ranges>\n"
        "#include <type_traits>\n"
        "static_assert(std::is_same_v<decltype(std::ranges::find(std::declval<int (&)[4]>(), 0)), int*>);\n"
        "static_assert(std::is_same_v<decltype(std::ranges::find(std::declval<const int (&)[4]>(), 0)), "
        "const int*>);\n"
        "constexpr int ak[4] = {1, 2, 3, 4};\n"
        "static_assert(std::ranges::find(ak, 1) == ak);\n"
        "static_assert(std::ranges::find(ak, 3) == ak + 2);\n"
        "static_assert(std::ranges::find(ak, 4) == ak + 3);\n"
        "static_assert(std::ranges::find(ak, 9) == ak + 4);\n"
        "int* f(int (&a)[4], int v) { return std::ranges::find(a, v); }\n"
        "const int* g(const int (&a)[4], int v) { return std::ranges::find(a, v); }\n"
        "int** h(int* (&a)[2]) { return std::ranges::find(a, nullptr); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_arr", find_arr_src, true));

    const char* find_ptr_src =
        "#include <ranges>\n"
        "int* f(int* p, int v) { return std::ranges::find(p, v); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_ptr", find_ptr_src, true));

    const char* find_pbound_src =
        "#include <ranges>\n"
        "int* f(int (*p)[4], int v) { return std::ranges::find(p, v); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_pbound", find_pbound_src, true));

    const char* find_mem_src =
        "#include <ranges>\n"
        "struct Has { int size() const { return 4; } };\n"
        "int* f(Has& h, int v) { return std::ranges::find(h, v); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_mem", find_mem_src, true));

    const char* find_if_arr_src =
        "#include <ranges>\n"
        "#include <type_traits>\n"
        "constexpr bool is_one(const int& x) { return x == 1; }\n"
        "constexpr bool is_three(const int& x) { return x == 3; }\n"
        "constexpr bool is_four(const int& x) { return x == 4; }\n"
        "constexpr bool is_nine(const int& x) { return x == 9; }\n"
        "static_assert(std::is_same_v<decltype(std::ranges::find_if(std::declval<int (&)[4]>(), is_one)), int*>);\n"
        "static_assert(std::is_same_v<decltype(std::ranges::find_if(std::declval<const int (&)[4]>(), is_one)), "
        "const int*>);\n"
        "constexpr int ak[4] = {1, 2, 3, 4};\n"
        "static_assert(std::ranges::find_if(ak, is_one) == ak);\n"
        "static_assert(std::ranges::find_if(ak, is_three) == ak + 2);\n"
        "static_assert(std::ranges::find_if(ak, is_four) == ak + 3);\n"
        "static_assert(std::ranges::find_if(ak, is_nine) == ak + 4);\n"
        "int* f(int (&a)[4], bool (*p)(const int&)) { return std::ranges::find_if(a, p); }\n"
        "const int* g(const int (&a)[4], bool (*p)(const int&)) { return std::ranges::find_if(a, p); }\n"
        "int** h(int* (&a)[2], bool (*p)(int* const&)) { return std::ranges::find_if(a, p); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_arr", find_if_arr_src, false));
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_arr", find_if_arr_src, true));

    const char* find_if_ptr_src =
        "#include <ranges>\n"
        "int* f(int* p, bool (*pred)(const int&)) { return std::ranges::find_if(p, pred); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_ptr", find_if_ptr_src, true));

    const char* find_if_pbound_src =
        "#include <ranges>\n"
        "int* f(int (*p)[4], bool (*pred)(const int&)) { return std::ranges::find_if(p, pred); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_pbound", find_if_pbound_src, true));

    const char* find_if_pair_src =
        "#include <ranges>\n"
        "int* f(int* b, int* e, bool (*pred)(const int&)) { return std::ranges::find_if(b, e, pred); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_pair", find_if_pair_src, false));
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_pair", find_if_pair_src, true));

    const char* find_if_mem_src =
        "#include <ranges>\n"
        "struct Has { int size() const { return 4; } };\n"
        "int* f(Has& h, bool (*pred)(const int&)) { return std::ranges::find_if(h, pred); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_mem", find_if_mem_src, true));

    const char* find_if_lam_src =
        "#include <ranges>\n"
        "int* f(int (&a)[4]) { return std::ranges::find_if(a, [](int x) { return x == 0; }); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_lam", find_if_lam_src, false));
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_lam", find_if_lam_src, true));

    const char* find_if_lam_ref_src =
        "#include <ranges>\n"
        "int* f(int (&a)[4]) { return std::ranges::find_if(a, [](const int& x) { return x == 0; }); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_lam_ref", find_if_lam_ref_src, false));
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_lam_ref", find_if_lam_ref_src, true));

    const char* find_if_val_src =
        "#include <ranges>\n"
        "bool is_zero(int x) { return x == 0; }\n"
        "int* f(int (&a)[4]) { return std::ranges::find_if(a, is_zero); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_val", find_if_val_src, false));
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_val", find_if_val_src, true));

    const char* find_if_null_src =
        "#include <ranges>\n"
        "constexpr int nullp() {\n"
        "  int a[3] = {1, 2, 3};\n"
        "  return *std::ranges::find_if(a, static_cast<bool (*)(const int&)>(nullptr));\n"
        "}\n"
        "static_assert(nullp() == 0);\n";
    std::string find_if_null_err;
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_null", find_if_null_src, true, &find_if_null_err));
    CHECK(find_if_null_err.find("__prism_check") != std::string::npos);

    const char* find_if_not_arr_src =
        "#include <ranges>\n"
        "#include <type_traits>\n"
        "constexpr bool is_one(const int& x) { return x == 1; }\n"
        "constexpr bool is_two(const int& x) { return x == 2; }\n"
        "constexpr bool always(const int&) { return true; }\n"
        "static_assert(std::is_same_v<decltype(std::ranges::find_if_not(std::declval<int (&)[4]>(), is_one)), "
        "int*>);\n"
        "static_assert(std::is_same_v<decltype(std::ranges::find_if_not(std::declval<const int (&)[4]>(), is_one)), "
        "const int*>);\n"
        "constexpr int ak[4] = {1, 2, 3, 4};\n"
        "static_assert(std::ranges::find_if_not(ak, is_one) == ak + 1);\n"
        "static_assert(std::ranges::find_if_not(ak, is_two) == ak);\n"
        "static_assert(std::ranges::find_if_not(ak, always) == ak + 4);\n"
        "int* f(int (&a)[4], bool (*p)(const int&)) { return std::ranges::find_if_not(a, p); }\n"
        "const int* g(const int (&a)[4], bool (*p)(const int&)) { return std::ranges::find_if_not(a, p); }\n"
        "int** h(int* (&a)[2], bool (*p)(int* const&)) { return std::ranges::find_if_not(a, p); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_arr", find_if_not_arr_src, false));
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_arr", find_if_not_arr_src, true));

    const char* find_if_not_ptr_src =
        "#include <ranges>\n"
        "int* f(int* p, bool (*pred)(const int&)) { return std::ranges::find_if_not(p, pred); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_ptr", find_if_not_ptr_src, true));

    const char* find_if_not_pbound_src =
        "#include <ranges>\n"
        "int* f(int (*p)[4], bool (*pred)(const int&)) { return std::ranges::find_if_not(p, pred); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_pbound", find_if_not_pbound_src, true));

    const char* find_if_not_pair_src =
        "#include <ranges>\n"
        "int* f(int* b, int* e, bool (*pred)(const int&)) { return std::ranges::find_if_not(b, e, pred); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_pair", find_if_not_pair_src, false));
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_pair", find_if_not_pair_src, true));

    const char* find_if_not_mem_src =
        "#include <ranges>\n"
        "struct Has { int size() const { return 4; } };\n"
        "int* f(Has& h, bool (*pred)(const int&)) { return std::ranges::find_if_not(h, pred); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_mem", find_if_not_mem_src, true));

    const char* find_if_not_lam_src =
        "#include <ranges>\n"
        "int* f(int (&a)[4]) { return std::ranges::find_if_not(a, [](int x) { return x == 0; }); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_lam", find_if_not_lam_src, false));
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_lam", find_if_not_lam_src, true));

    const char* find_if_not_lam_ref_src =
        "#include <ranges>\n"
        "int* f(int (&a)[4]) { return std::ranges::find_if_not(a, [](const int& x) { return x == 0; }); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_lam_ref", find_if_not_lam_ref_src, false));
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_lam_ref", find_if_not_lam_ref_src, true));

    const char* find_if_not_val_src =
        "#include <ranges>\n"
        "bool is_zero(int x) { return x == 0; }\n"
        "int* f(int (&a)[4]) { return std::ranges::find_if_not(a, is_zero); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_val", find_if_not_val_src, false));
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_val", find_if_not_val_src, true));

    const char* find_if_not_null_src =
        "#include <ranges>\n"
        "constexpr int nullp() {\n"
        "  int a[3] = {1, 2, 3};\n"
        "  return *std::ranges::find_if_not(a, static_cast<bool (*)(const int&)>(nullptr));\n"
        "}\n"
        "static_assert(nullp() == 0);\n";
    std::string find_if_not_null_err;
    CHECK_FALSE(
        compile_snippet(c.cxx, c.tmp, "ranges_find_if_not_null", find_if_not_null_src, true, &find_if_not_null_err));
    CHECK(find_if_not_null_err.find("__prism_check") != std::string::npos);

    const char* count_if_arr_src =
        "#include <ranges>\n"
        "#include <algorithm>\n"
        "#include <cstddef>\n"
        "#include <type_traits>\n"
        "constexpr bool is_one(const int& x) { return x == 1; }\n"
        "constexpr bool is_four(const int& x) { return x == 4; }\n"
        "constexpr bool is_nine(const int& x) { return x == 9; }\n"
        "constexpr bool always(const int&) { return true; }\n"
        "static_assert(std::is_same_v<decltype(std::ranges::count_if(std::declval<int (&)[4]>(), is_one)), "
        "std::ptrdiff_t>);\n"
        "static_assert(std::is_same_v<decltype(std::ranges::count_if(std::declval<const int (&)[4]>(), is_one)), "
        "std::ptrdiff_t>);\n"
        "constexpr int ak[4] = {1, 2, 4, 4};\n"
        "static_assert(std::ranges::count_if(ak, is_one) == 1);\n"
        "static_assert(std::ranges::count_if(ak, is_four) == 2);\n"
        "static_assert(std::ranges::count_if(ak, is_nine) == 0);\n"
        "static_assert(std::ranges::count_if(ak, always) == 4);\n"
        "std::ptrdiff_t f(int (&a)[4], bool (*p)(const int&)) { return std::ranges::count_if(a, p); }\n"
        "std::ptrdiff_t g(const int (&a)[4], bool (*p)(const int&)) { return std::ranges::count_if(a, p); }\n"
        "std::ptrdiff_t h(int* (&a)[2], bool (*p)(int* const&)) { return std::ranges::count_if(a, p); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_count_if_arr", count_if_arr_src, false));
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_count_if_arr", count_if_arr_src, true));

    // <ranges> alone: libstdc++ declares ranges::count_if in <algorithm>, and so
    // does the model. Neither accepts this.
    const char* count_if_ranges_only_src =
        "#include <ranges>\n"
        "#include <cstddef>\n"
        "std::ptrdiff_t f(int (&a)[4], bool (*pred)(const int&)) { return std::ranges::count_if(a, pred); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_count_if_ranges_only", count_if_ranges_only_src, false));
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_count_if_ranges_only", count_if_ranges_only_src, true));

    const char* count_if_ptr_src =
        "#include <ranges>\n"
        "#include <algorithm>\n"
        "#include <cstddef>\n"
        "std::ptrdiff_t f(int* p, bool (*pred)(const int&)) { return std::ranges::count_if(p, pred); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_count_if_ptr", count_if_ptr_src, true));

    const char* count_if_pbound_src =
        "#include <ranges>\n"
        "#include <algorithm>\n"
        "#include <cstddef>\n"
        "std::ptrdiff_t f(int (*p)[4], bool (*pred)(const int&)) { return std::ranges::count_if(p, pred); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_count_if_pbound", count_if_pbound_src, true));

    const char* count_if_pair_src =
        "#include <ranges>\n"
        "#include <algorithm>\n"
        "#include <cstddef>\n"
        "std::ptrdiff_t f(int* b, int* e, bool (*pred)(const int&)) { return std::ranges::count_if(b, e, pred); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_count_if_pair", count_if_pair_src, false));
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_count_if_pair", count_if_pair_src, true));

    const char* count_if_mem_src =
        "#include <ranges>\n"
        "#include <algorithm>\n"
        "#include <cstddef>\n"
        "struct Has { int size() const { return 4; } };\n"
        "std::ptrdiff_t f(Has& h, bool (*pred)(const int&)) { return std::ranges::count_if(h, pred); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_count_if_mem", count_if_mem_src, true));

    const char* count_if_lam_src =
        "#include <ranges>\n"
        "#include <algorithm>\n"
        "#include <cstddef>\n"
        "std::ptrdiff_t f(int (&a)[4]) { return std::ranges::count_if(a, [](int x) { return x == 0; }); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_count_if_lam", count_if_lam_src, false));
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_count_if_lam", count_if_lam_src, true));

    const char* count_if_lam_ref_src =
        "#include <ranges>\n"
        "#include <algorithm>\n"
        "#include <cstddef>\n"
        "std::ptrdiff_t f(int (&a)[4]) { return std::ranges::count_if(a, [](const int& x) { return x == 0; }); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_count_if_lam_ref", count_if_lam_ref_src, false));
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_count_if_lam_ref", count_if_lam_ref_src, true));

    const char* count_if_val_src =
        "#include <ranges>\n"
        "#include <algorithm>\n"
        "#include <cstddef>\n"
        "bool is_zero(int x) { return x == 0; }\n"
        "std::ptrdiff_t f(int (&a)[4]) { return std::ranges::count_if(a, is_zero); }\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_count_if_val", count_if_val_src, false));
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_count_if_val", count_if_val_src, true));

    const char* count_if_null_src =
        "#include <ranges>\n"
        "#include <algorithm>\n"
        "constexpr int nullp() {\n"
        "  int a[3] = {1, 2, 3};\n"
        "  return static_cast<int>(std::ranges::count_if(a, static_cast<bool (*)(const int&)>(nullptr)));\n"
        "}\n"
        "static_assert(nullp() == 0);\n";
    std::string count_if_null_err;
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_count_if_null", count_if_null_src, true, &count_if_null_err));
    CHECK(count_if_null_err.find("__prism_check") != std::string::npos);

    const char* empty_ptr_src =
        "#include <ranges>\n"
        "bool f(int* p) { return std::ranges::empty(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_empty_ptr", empty_ptr_src, true));

    const char* empty_pbound_src =
        "#include <ranges>\n"
        "bool f(int (*p)[4]) { return std::ranges::empty(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_empty_pbound", empty_pbound_src, true));

    const char* empty_rval_src =
        "#include <ranges>\n"
        "struct Flag { bool empty() const { return true; } };\n"
        "bool f() { return std::ranges::empty(Flag{}); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_empty_rval", empty_rval_src, true));

    const char* empty_mut_src =
        "#include <ranges>\n"
        "struct Mut { bool empty() { return true; } };\n"
        "bool f() { Mut m; return std::ranges::empty(m); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_empty_mut", empty_mut_src, true));
}
