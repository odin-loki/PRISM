// std::forward_list operator!= for two forward_list<T, Alloc> of the same
// type is !(__x == __y). The body does not walk elements. Two empty lists
// compare not-equal as false and do not load a node, because operator==
// does not enter its loop when begin and end compare equal. A mismatch
// stops at the first difference inside operator==. The function does not
// return T{}. The header is not included here: this binary already uses
// the standard library in other translation units. This is not PROVED and
// not CLEAN. One line in _prism_test_sources.

#include <doctest/doctest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "forward_list"; }

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

bool has_ident(std::string_view s, std::string_view id) {
    for (std::size_t i = 0; (i = s.find(id, i)) != std::string_view::npos; i += id.size()) {
        const bool left = i == 0 || !(std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_');
        const std::size_t j = i + id.size();
        const bool right = j >= s.size() || !(std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_');
        if (left && right) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("cxx forward_list model: operator!= is the negation of operator==") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#ifndef _PRISM_MODEL_FORWARD_LIST") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_FORWARD_LIST 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(count_sub(src, "#include <memory>") == 1);
    CHECK(count_sub(src, "__prism_check(!this->empty() ? 1 : 0, \"FUNC-CONTRACT\",") == 3);
    CHECK(count_sub(src, "__builtin_trap()") == 3);

    const std::string eq_sig =
        "bool operator==(const forward_list<_Tp, _Alloc>& __x, const forward_list<_Tp, _Alloc>& __y)";
    const std::string ne_sig =
        "bool operator!=(const forward_list<_Tp, _Alloc>& __x, const forward_list<_Tp, _Alloc>& __y)";
    const std::string cmp_sig =
        "__detail::__synth3way_t<_Tp> operator<=>(const forward_list<_Tp, _Alloc>& __x,";
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
    CHECK(src.find("friend bool operator!=(const forward_list") == std::string::npos);
    CHECK(src.find("operator!=(const forward_list<_Tp, _Alloc>&, const forward_list<") == std::string::npos);

    const std::string eq = norm_nl(body_of(src, eq_sig));
    CHECK(eq ==
          "{\n"
          "    auto __i1 = __x.begin();\n"
          "    auto __i2 = __y.begin();\n"
          "    auto __e1 = __x.end();\n"
          "    auto __e2 = __y.end();\n"
          "    for (; __i1 != __e1 && __i2 != __e2; ++__i1, (void)++__i2)\n"
          "        if (!(*__i1 == *__i2)) return false;\n"
          "    return __i1 == __e1 && __i2 == __e2;\n"
          "}");
    const auto loop = eq.find("for (; __i1 != __e1 && __i2 != __e2; ++__i1, (void)++__i2)");
    const auto mismatch = eq.find("if (!(*__i1 == *__i2)) return false;");
    REQUIRE(loop != std::string::npos);
    REQUIRE(mismatch != std::string::npos);
    CHECK(loop < mismatch);
    CHECK(eq.find("*__i1") != std::string::npos);
    CHECK(eq.substr(0, loop).find("*__i1") == std::string::npos);

    const std::string ne = norm_nl(body_of(src, ne_sig));
    REQUIRE(ne == "{\n    return !(__x == __y);\n}");
    CHECK(ne.find("!=") == std::string::npos);
    CHECK(count_sub(ne, "==") == 1);
    CHECK(ne.find("for") == std::string::npos);
    CHECK(ne.find("begin") == std::string::npos);
    CHECK(ne.find("end") == std::string::npos);
    CHECK(ne.find("++") == std::string::npos);
    CHECK(ne.find("*") == std::string::npos);
    CHECK(ne.find("_M_") == std::string::npos);
    CHECK(ne.find("_S_val") == std::string::npos);
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
    CHECK(note.find("do not load a node") != std::string::npos);
    CHECK(note.find("first difference") != std::string::npos);
    CHECK(note.find("does not return T{}") != std::string::npos);

    const std::string cmp = norm_nl(body_of(src, "operator<=>(const forward_list<_Tp, _Alloc>& __x,"));
    CHECK(cmp ==
          "{\n"
          "    return std::lexicographical_compare_three_way(__x.begin(), __x.end(), __y.begin(), __y.end(),\n"
          "                                                  __detail::__synth3way);\n"
          "}");

    CHECK(norm_nl(body_of(src, "reference front()")) ==
          "{\n"
          "        __prism_check(!this->empty() ? 1 : 0, \"FUNC-CONTRACT\",\n"
          "                      \"std::forward_list::front on an empty list\");\n"
          "        if (this->empty()) __builtin_trap();\n"
          "        return *_S_val(_M_head._M_next);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "const_reference front() const")) ==
          "{\n"
          "        __prism_check(!this->empty() ? 1 : 0, \"FUNC-CONTRACT\",\n"
          "                      \"std::forward_list::front on an empty list\");\n"
          "        if (this->empty()) __builtin_trap();\n"
          "        return *_S_val(_M_head._M_next);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "void pop_front()")) ==
          "{\n"
          "        __prism_check(!this->empty() ? 1 : 0, \"FUNC-CONTRACT\",\n"
          "                      \"std::forward_list::pop_front on an empty list\");\n"
          "        if (this->empty()) __builtin_trap();\n"
          "        _M_erase_after(&_M_head);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "reference emplace_front(_Args&&... __args)")) ==
          "{\n"
          "        return *emplace_after(cbefore_begin(), std::forward<_Args>(__args)...);\n"
          "    }");
    CHECK(body_of(src, "void push_front(const value_type& __x)") == "{ emplace_front(__x); }");
    CHECK(body_of(src, "void push_front(value_type&& __x)") == "{ emplace_front(std::move(__x)); }");
    CHECK(body_of(src, "bool empty() const noexcept") == "{ return _M_head._M_next == nullptr; }");
    CHECK(body_of(src, "void clear() noexcept") == "{ this->_M_clear(); }");
    CHECK(norm_nl(body_of(src, "void swap(forward_list& __x) noexcept")) ==
          "{\n"
          "        std::swap(_M_head._M_next, __x._M_head._M_next);\n"
          "        this->_M_adopt();\n"
          "        __x._M_adopt();\n"
          "    }");
    CHECK(src.find("iterator begin() noexcept { return iterator(_M_head._M_next); }") != std::string::npos);
    CHECK(src.find("iterator end() noexcept { return iterator(nullptr); }") != std::string::npos);

    const std::string code = strip_comments_and_strings(src);
    CHECK(count_sub(code, "operator!=") == 1);
    CHECK(count_sub(code, ne_sig) == 1);
    CHECK(code.find("operator!=(const _Self&") == std::string::npos);
    CHECK(code.find("operator!=(const iterator&") == std::string::npos);
    CHECK(count_sub(code, "friend bool operator==(const _Self& __x, const _Self& __y) noexcept { return __x._M_node == "
                           "__y._M_node; }") == 2);
    CHECK_FALSE(has_ident(code, "splice_after"));
    CHECK_FALSE(has_ident(code, "merge"));
}
