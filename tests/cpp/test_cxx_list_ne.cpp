// std::list operator!= for two list<T, Alloc> of the same type is
// !(__x == __y). The body does not walk elements and does not call
// begin(), end(), or size(). Two empty lists compare not-equal as false.
// operator== compares the stored sizes first and returns false before
// begin() when the sizes differ. When both sizes are 0, its loop does not
// load an element because begin and end compare equal. A mismatch of equal
// length stops at the first difference inside operator==. The function does
// not return T{}. The header is not included here: this binary already uses
// the standard library in other translation units. This is not PROVED and
// not CLEAN. One line in _prism_test_sources.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "list"; }

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

TEST_CASE("cxx list model: operator!= is the negation of operator==") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#ifndef _PRISM_MODEL_LIST") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_LIST 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("__prism_check") == std::string::npos);
    CHECK(src.find("__builtin_trap") == std::string::npos);

    const std::string eq_sig = "bool operator==(const list<_Tp, _Alloc>& __x, const list<_Tp, _Alloc>& __y)";
    const std::string ne_sig = "bool operator!=(const list<_Tp, _Alloc>& __x, const list<_Tp, _Alloc>& __y)";
    const std::string cmp_sig = "__detail::__synth3way_t<_Tp> operator<=>(const list<_Tp, _Alloc>& __x,";
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
    CHECK(src.find("friend bool operator!=(const list") == std::string::npos);
    CHECK(src.find("operator!=(const list<_Tp, _Alloc>&, const list<") == std::string::npos);

    const std::string eq = norm_nl(body_of(src, eq_sig));
    CHECK(eq ==
          "{\n"
          "    if (__x.size() != __y.size()) return false;\n"
          "    auto __i1 = __x.begin();\n"
          "    auto __i2 = __y.begin();\n"
          "    for (auto __e = __x.end(); __i1 != __e; ++__i1, (void)++__i2)\n"
          "        if (!(*__i1 == *__i2)) return false;\n"
          "    return true;\n"
          "}");
    const auto size_at = eq.find("if (__x.size() != __y.size()) return false;");
    const auto begin_at = eq.find("__x.begin()");
    const auto loop = eq.find("for (auto __e = __x.end(); __i1 != __e; ++__i1, (void)++__i2)");
    const auto mismatch = eq.find("if (!(*__i1 == *__i2)) return false;");
    REQUIRE(size_at != std::string::npos);
    REQUIRE(begin_at != std::string::npos);
    REQUIRE(loop != std::string::npos);
    REQUIRE(mismatch != std::string::npos);
    CHECK(size_at < begin_at);
    CHECK(begin_at < loop);
    CHECK(loop < mismatch);
    CHECK(eq.substr(0, loop).find("*__i1") == std::string::npos);

    const std::string ne = norm_nl(body_of(src, ne_sig));
    REQUIRE(ne == "{\n    return !(__x == __y);\n}");
    CHECK(ne.find("!=") == std::string::npos);
    CHECK(count_sub(ne, "==") == 1);
    CHECK(ne.find("for") == std::string::npos);
    CHECK(ne.find("while") == std::string::npos);
    CHECK(ne.find("begin") == std::string::npos);
    CHECK(ne.find("end") == std::string::npos);
    CHECK(ne.find("size") == std::string::npos);
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
    CHECK(note.find("does not load an element") != std::string::npos);
    CHECK(note.find("stored sizes") != std::string::npos);
    CHECK(note.find("size mismatch") != std::string::npos);
    CHECK(note.find("before begin()") != std::string::npos);
    CHECK(note.find("begin and end compare equal") != std::string::npos);
    CHECK(note.find("first difference") != std::string::npos);
    CHECK(note.find("does not return T{}") != std::string::npos);
    CHECK(note.find("does not call begin()") != std::string::npos);

    const std::string cmp = norm_nl(body_of(src, "operator<=>(const list<_Tp, _Alloc>& __x,"));
    CHECK(cmp ==
          "{\n"
          "    return std::lexicographical_compare_three_way(__x.begin(), __x.end(), __y.begin(), __y.end(),\n"
          "                                                  __detail::__synth3way);\n"
          "}");

    CHECK(src.find("iterator begin() noexcept { return iterator(_M_head._M_next); }") != std::string::npos);
    CHECK(src.find("const_iterator begin() const noexcept { return const_iterator(_M_head._M_next); }") !=
          std::string::npos);
    CHECK(src.find("iterator end() noexcept { return iterator(&_M_head); }") != std::string::npos);
    CHECK(src.find("const_iterator end() const noexcept { return const_iterator(&_M_head); }") != std::string::npos);
    CHECK(src.find("[[nodiscard]] bool empty() const noexcept { return _M_head._M_next == &_M_head; }") !=
          std::string::npos);
    CHECK(src.find("[[nodiscard]] size_type size() const noexcept { return _M_size; }") != std::string::npos);

    CHECK(norm_nl(body_of(src, "reference front() noexcept")) ==
          "{\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        return *_S_val(_M_head._M_next);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "const_reference front() const noexcept")) ==
          "{\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        return *_S_val(_M_head._M_next);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "reference back() noexcept")) ==
          "{\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        return *_S_val(_M_head._M_prev);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "const_reference back() const noexcept")) ==
          "{\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        return *_S_val(_M_head._M_prev);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "void pop_front() noexcept")) ==
          "{\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        _M_erase_one(_M_head._M_next);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "void pop_back() noexcept")) ==
          "{\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        _M_erase_one(_M_head._M_prev);\n"
          "    }");
    CHECK(body_of(src, "void push_front(const value_type& __x)") == "{ emplace_front(__x); }");
    CHECK(body_of(src, "void push_front(value_type&& __x)") == "{ emplace_front(std::move(__x)); }");
    CHECK(body_of(src, "void push_back(const value_type& __x)") == "{ emplace_back(__x); }");
    CHECK(body_of(src, "void push_back(value_type&& __x)") == "{ emplace_back(std::move(__x)); }");
    CHECK(body_of(src, "iterator insert(const_iterator __pos, const value_type& __x)") ==
          "{ return emplace(__pos, __x); }");
    CHECK(body_of(src, "iterator insert(const_iterator __pos, value_type&& __x)") ==
          "{ return emplace(__pos, std::move(__x)); }");
    CHECK(norm_nl(body_of(src, "iterator insert(const_iterator __pos, size_type __n, const value_type& __x)")) ==
          "{\n"
          "        _Link* __p = const_cast<_Link*>(__pos._M_node);\n"
          "        _M_check_pos(__p);\n"
          "        if (__n == 0) return iterator(__p);\n"
          "        _Prism_list_head __tmp;\n"
          "        size_type __built = 0;\n"
          "        try {\n"
          "            for (; __built < __n; ++__built) {\n"
          "                _Node* __node = _M_create(__x);\n"
          "                __node->_M_hook(&__tmp);\n"
          "                __node->_M_owner = &__tmp;\n"
          "            }\n"
          "        } catch (...) {\n"
          "            _M_drop_owned(&__tmp);\n"
          "            throw;\n"
          "        }\n"
          "        return _M_splice_new(__p, __tmp, __built);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "iterator insert(const_iterator __pos, _InputIterator __first, _InputIterator __last)")) ==
          "{\n"
          "        _Link* __p = const_cast<_Link*>(__pos._M_node);\n"
          "        _M_check_pos(__p);\n"
          "        _Prism_list_head __tmp;\n"
          "        size_type __built = 0;\n"
          "        try {\n"
          "            for (; __first != __last; ++__first, ++__built) {\n"
          "                _Node* __node = _M_create(*__first);\n"
          "                __node->_M_hook(&__tmp);\n"
          "                __node->_M_owner = &__tmp;\n"
          "            }\n"
          "        } catch (...) {\n"
          "            _M_drop_owned(&__tmp);\n"
          "            throw;\n"
          "        }\n"
          "        return _M_splice_new(__p, __tmp, __built);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "iterator insert(const_iterator __pos, initializer_list<value_type> __l)")) ==
          "{\n"
          "        return insert(__pos, __l.begin(), __l.end());\n"
          "    }");
    CHECK(norm_nl(body_of(src, "iterator erase(const_iterator __pos) noexcept")) ==
          "{\n"
          "        _Link* __p = const_cast<_Link*>(__pos._M_node);\n"
          "        _M_check_elem(__p);\n"
          "        _Link* __nxt = __p->_M_next;\n"
          "        _M_erase_one(__p);\n"
          "        return iterator(__nxt);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "iterator erase(const_iterator __first, const_iterator __last) noexcept")) ==
          "{\n"
          "        _Link* __f = const_cast<_Link*>(__first._M_node);\n"
          "        _Link* __l = const_cast<_Link*>(__last._M_node);\n"
          "        _M_check_pos(__f);\n"
          "        _M_check_pos(__l);\n"
          "        while (__f != __l) {\n"
          "            _Link* __nxt = __f->_M_next;\n"
          "            _M_erase_one(__f);\n"
          "            __f = __nxt;\n"
          "        }\n"
          "        return iterator(__l);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "void clear() noexcept")) ==
          "{\n"
          "        _M_drop_owned(&_M_head);\n"
          "        _M_size = 0;\n"
          "    }");
    CHECK(norm_nl(body_of(src, "void swap(list& __x) noexcept")) ==
          "{\n"
          "        _Prism_list_swap(_M_head, __x._M_head);\n"
          "        std::swap(_M_size, __x._M_size);\n"
          "        _M_adopt();\n"
          "        __x._M_adopt();\n"
          "    }");
    CHECK(norm_nl(body_of(src, "void swap(list<_Tp, _Alloc>& __x, list<_Tp, _Alloc>& __y) noexcept")) ==
          "{\n"
          "    __x.swap(__y);\n"
          "}");

    const std::string code = strip_comments_and_strings(src);
    CHECK(count_sub(code, "operator!=") == 1);
    CHECK(count_sub(code, ne_sig) == 1);
    CHECK(code.find("operator!=(const _Self&") == std::string::npos);
    CHECK(code.find("operator!=(const iterator") == std::string::npos);
    CHECK(code.find("operator!=(iterator") == std::string::npos);
    CHECK(code.find("operator!=(const const_iterator") == std::string::npos);
    CHECK(count_sub(code, "friend bool operator==(const _Self& __x, const _Self& __y) noexcept { return __x._M_node == "
                           "__y._M_node; }") == 2);

    const std::string cmake = slurp(repo() / "CMakeLists.txt");
    CHECK(count_sub(cmake, "tests/cpp/test_cxx_list_ne.cpp") == 1);
    CHECK(cmake.find("tests/cpp/test_cxx_list.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_cxx_vector_ne.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_cxx_forward_list_ne.cpp") != std::string::npos);
}
