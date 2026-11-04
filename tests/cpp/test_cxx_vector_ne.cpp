// std::vector operator!= for two vector<T, Alloc> of the same type is
// !(__x == __y). The body does not walk elements and does not call begin()
// or end(). Two empty vectors compare not-equal as false. operator==
// compares sizes first; when both sizes are 0 it still calls std::equal,
// and that empty range does not load an element. A size mismatch makes
// operator== false before std::equal. A mismatch of equal length stops at
// the first difference inside operator==. The function does not return
// T{}. The header is not included here: this binary already uses the
// standard library in other translation units. This is not PROVED and not
// CLEAN. One line in _prism_test_sources.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "vector"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int count_sub(std::string_view s, std::string_view n) {
    int c = 0;
    for (std::size_t i = 0; (i = s.find(n, i)) != std::string_view::npos; i += n.size()) ++c;
    return c;
}

std::string norm_nl(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\r') out.push_back(c);
    return out;
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

std::string strip_comments_and_strings(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const std::size_t n = in.size();
    for (std::size_t i = 0; i < n;) {
        if (in[i] == '/' && i + 1 < n && in[i + 1] == '/') {
            i += 2;
            while (i < n && in[i] != '\n') ++i;
            continue;
        }
        if (in[i] == '/' && i + 1 < n && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(in[i] == '*' && in[i + 1] == '/')) ++i;
            if (i + 1 < n) i += 2;
            else i = n;
            out.push_back(' ');
            continue;
        }
        if (in[i] == '"') {
            out.push_back('"');
            ++i;
            while (i < n && in[i] != '"') {
                if (in[i] == '\\' && i + 1 < n) i += 2;
                else ++i;
            }
            if (i < n) ++i;
            out.push_back('"');
            continue;
        }
        out.push_back(in[i++]);
    }
    return out;
}

}  // namespace

TEST_CASE("cxx vector model: operator!= is the negation of operator==") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#ifndef _PRISM_MODEL_VECTOR") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_VECTOR 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    const auto vb = src.find("class vector<bool, _Alloc>;");
    REQUIRE(vb != std::string::npos);
    CHECK(src.find("class vector<bool, _Alloc> {") == std::string::npos);
    const auto vb_line = src.rfind('\n', vb);
    REQUIRE(vb_line != std::string::npos);
    const auto vb_prev = src.rfind('\n', vb_line - 1);
    std::string vb_prev_line =
        src.substr(vb_prev == std::string::npos ? 0 : vb_prev + 1, vb_line - (vb_prev == std::string::npos ? 0 : vb_prev + 1));
    if (!vb_prev_line.empty() && vb_prev_line.back() == '\r') vb_prev_line.pop_back();
    CHECK(vb_prev_line == "template <typename _Alloc>");

    const std::string eq_sig = "bool operator==(const vector<_Tp, _Alloc>& __x, const vector<_Tp, _Alloc>& __y)";
    const std::string ne_sig = "bool operator!=(const vector<_Tp, _Alloc>& __x, const vector<_Tp, _Alloc>& __y)";
    const std::string cmp_sig = "__detail::__synth3way_t<_Tp> operator<=>(const vector<_Tp, _Alloc>& __x,";
    const auto eq_at = src.find(eq_sig);
    const auto ne_at = src.find(ne_sig);
    const auto cmp_at = src.find(cmp_sig);
    REQUIRE(eq_at != std::string::npos);
    REQUIRE(ne_at != std::string::npos);
    REQUIRE(cmp_at != std::string::npos);
    CHECK(eq_at < ne_at);
    CHECK(ne_at < cmp_at);
    CHECK(count_sub(src, eq_sig) == 1);
    CHECK(count_sub(src, ne_sig) == 1);
    CHECK(src.find("friend bool operator!=(const vector") == std::string::npos);
    CHECK(src.find("operator!=(const vector<_Tp, _Alloc>&, const vector<") == std::string::npos);

    const std::string eq = norm_nl(body_of(src, eq_sig));
    CHECK(eq ==
          "{\n"
          "    return __x.size() == __y.size() && std::equal(__x.begin(), __x.end(), __y.begin());\n"
          "}");
    const auto size_at = eq.find("__x.size() == __y.size()");
    const auto equal_at = eq.find("std::equal(__x.begin(), __x.end(), __y.begin())");
    REQUIRE(size_at != std::string::npos);
    REQUIRE(equal_at != std::string::npos);
    CHECK(size_at < equal_at);
    CHECK(eq.find("&&") != std::string::npos);

    const std::string ne = norm_nl(body_of(src, ne_sig));
    REQUIRE(ne == "{\n    return !(__x == __y);\n}");
    CHECK(ne.find("!=") == std::string::npos);
    CHECK(count_sub(ne, "==") == 1);
    CHECK(ne.find("for") == std::string::npos);
    CHECK(ne.find("begin") == std::string::npos);
    CHECK(ne.find("end") == std::string::npos);
    CHECK(ne.find("std::equal") == std::string::npos);
    CHECK(ne.find("++") == std::string::npos);
    CHECK(ne.find("*") == std::string::npos);
    CHECK(ne.find("_M_") == std::string::npos);
    CHECK(ne.find("T{}") == std::string::npos);
    CHECK(ne.find("value_type{}") == std::string::npos);
    CHECK(ne.find("value_type()") == std::string::npos);
    CHECK(ne.find("_Tp{}") == std::string::npos);
    CHECK(ne.find("_Tp()") == std::string::npos);
    CHECK(ne.find("return T") == std::string::npos);
    CHECK(ne.find("return {}") == std::string::npos);
    CHECK(ne.find("return false") == std::string::npos);
    CHECK(ne.find("return 0") == std::string::npos);
    CHECK(ne.find("__prism_check") == std::string::npos);
    CHECK(ne.find("__builtin_trap") == std::string::npos);

    const auto note = src.substr(eq_at, ne_at - eq_at);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);
    CHECK(note.find("does not load an element") != std::string::npos);
    CHECK(note.find("std::equal") != std::string::npos);
    CHECK(note.find("size mismatch") != std::string::npos);
    CHECK(note.find("first difference") != std::string::npos);
    CHECK(note.find("does not return T{}") != std::string::npos);
    CHECK(note.find("does not call begin()") != std::string::npos);

    const std::string cmp = norm_nl(body_of(src, "operator<=>(const vector<_Tp, _Alloc>& __x,"));
    CHECK(cmp ==
          "{\n"
          "    return std::lexicographical_compare_three_way(__x.begin(), __x.end(), __y.begin(), __y.end(),\n"
          "                                                  __detail::__synth3way);\n"
          "}");
    CHECK(norm_nl(body_of(src, "void swap(vector& __x) noexcept")) ==
          "{\n"
          "        std::swap(_M_start, __x._M_start);\n"
          "        std::swap(_M_finish, __x._M_finish);\n"
          "        std::swap(_M_end_of_storage, __x._M_end_of_storage);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "void clear() noexcept")) ==
          "{\n"
          "        _M_destroy(_M_start, _M_finish);\n"
          "        _M_finish = _M_start;\n"
          "    }");
    CHECK(norm_nl(body_of(src, "void swap(vector<_Tp, _Alloc>& __x, vector<_Tp, _Alloc>& __y) noexcept")) ==
          "{\n"
          "    __x.swap(__y);\n"
          "}");

    const std::string code = strip_comments_and_strings(src);
    CHECK(count_sub(code, "operator!=") == 1);
    CHECK(count_sub(code, ne_sig) == 1);
    CHECK(code.find("operator!=(const iterator") == std::string::npos);
    CHECK(code.find("operator!=(iterator") == std::string::npos);
    CHECK(code.find("operator!=(const const_iterator") == std::string::npos);
    CHECK(code.find("class vector<bool, _Alloc>;") != std::string::npos);
    CHECK(code.find("class vector<bool, _Alloc> {") == std::string::npos);
}
