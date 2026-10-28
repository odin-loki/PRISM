// std::deque operator!= for two deque<T, Alloc> of the same type is
// !(__x == __y). The body does not walk elements and does not call
// begin(), end(), or size(). Two empty deques compare not-equal as false.
// operator== compares sizes first; when both sizes are 0 it still calls
// std::equal, and that empty range does not load an element. A size
// mismatch makes operator== false before std::equal. A mismatch of equal
// length stops at the first difference inside operator==. The function does
// not return T{}. The header is not included here: this binary already uses
// the standard library in other translation units. This is not PROVED and
// not CLEAN. One line in _prism_test_sources.
//
// The model header is the extensionless file src/prism/pir/models/cxx/deque.
// It is a regular file, not a directory: a directory of that name would make
// #include <deque> fail with EISDIR.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "deque"; }

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

TEST_CASE("cxx deque model: operator!= is the negation of operator==") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#ifndef _PRISM_MODEL_DEQUE") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_DEQUE 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("__prism_check") == std::string::npos);
    CHECK(src.find("__builtin_trap") == std::string::npos);

    const std::string eq_sig = "bool operator==(const deque<_Tp, _Alloc>& __x, const deque<_Tp, _Alloc>& __y)";
    const std::string ne_sig = "bool operator!=(const deque<_Tp, _Alloc>& __x, const deque<_Tp, _Alloc>& __y)";
    const std::string cmp_sig = "__detail::__synth3way_t<_Tp> operator<=>(const deque<_Tp, _Alloc>& __x,";
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
    CHECK(src.find("friend bool operator!=(const deque") == std::string::npos);
    CHECK(src.find("operator!=(const deque<_Tp, _Alloc>&, const deque<") == std::string::npos);

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
    CHECK(ne.find("while") == std::string::npos);
    CHECK(ne.find("begin") == std::string::npos);
    CHECK(ne.find("end") == std::string::npos);
    CHECK(ne.find("size") == std::string::npos);
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
    CHECK(note.find("or size()") != std::string::npos);

    const std::string cmp = norm_nl(body_of(src, "operator<=>(const deque<_Tp, _Alloc>& __x,"));
    CHECK(cmp ==
          "{\n"
          "    return std::lexicographical_compare_three_way(__x.begin(), __x.end(), __y.begin(), __y.end(),\n"
          "                                                  __detail::__synth3way);\n"
          "}");

    CHECK(src.find("iterator begin() noexcept { return _M_begin<false>(); }") != std::string::npos);
    CHECK(src.find("const_iterator begin() const noexcept { return _M_begin<true>(); }") != std::string::npos);
    CHECK(src.find("iterator end() noexcept { return _M_end<false>(); }") != std::string::npos);
    CHECK(src.find("const_iterator end() const noexcept { return _M_end<true>(); }") != std::string::npos);
    CHECK(src.find("[[nodiscard]] bool empty() const noexcept { return size() == 0; }") != std::string::npos);
    CHECK(src.find("size_type size() const noexcept { return _M_c ? _M_c->size : 0; }") != std::string::npos);
    CHECK(src.find("if (!_M_c->head) return _M_end<_C>();") != std::string::npos);

    CHECK(norm_nl(body_of(src, "reference operator[](size_type __n) noexcept")) ==
          "{\n"
          "        __glibcxx_assert(__n < this->size());\n"
          "        return *_M_nth(__n)->_M_ptr();\n"
          "    }");
    CHECK(norm_nl(body_of(src, "const_reference operator[](size_type __n) const noexcept")) ==
          "{\n"
          "        __glibcxx_assert(__n < this->size());\n"
          "        return *_M_nth(__n)->_M_ptr();\n"
          "    }");
    CHECK(norm_nl(body_of(src, "reference front() noexcept")) ==
          "{\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        return *_M_c->head->_M_ptr();\n"
          "    }");
    CHECK(norm_nl(body_of(src, "const_reference front() const noexcept")) ==
          "{\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        return *_M_c->head->_M_ptr();\n"
          "    }");
    CHECK(norm_nl(body_of(src, "reference back() noexcept")) ==
          "{\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        return *_M_c->tail->_M_ptr();\n"
          "    }");
    CHECK(norm_nl(body_of(src, "const_reference back() const noexcept")) ==
          "{\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        return *_M_c->tail->_M_ptr();\n"
          "    }");
    CHECK(body_of(src, "void push_front(const value_type& __x)") == "{ emplace_front(__x); }");
    CHECK(body_of(src, "void push_front(value_type&& __x)") == "{ emplace_front(std::move(__x)); }");
    CHECK(body_of(src, "void push_back(const value_type& __x)") == "{ emplace_back(__x); }");
    CHECK(body_of(src, "void push_back(value_type&& __x)") == "{ emplace_back(std::move(__x)); }");
    CHECK(norm_nl(body_of(src, "void pop_front() noexcept")) ==
          "{\n"
          "        _M_bind();\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        const bool __kill_end = _M_c->size == 1;\n"
          "        _End* __neu = __kill_end ? _S_new_end() : nullptr;\n"
          "        _Node* __n = _M_c->head;\n"
          "        _Node* __next = __n->_M_next;\n"
          "        if (__next)\n"
          "            __next->_M_prev = nullptr;\n"
          "        else\n"
          "            _M_c->tail = nullptr;\n"
          "        _M_c->head = __next;\n"
          "        --_M_c->size;\n"
          "        _S_delete_node(__n);\n"
          "        if (__kill_end) {\n"
          "            ::operator delete(_M_c->endc);\n"
          "            _M_c->endc = __neu;\n"
          "        }\n"
          "    }");
    CHECK(norm_nl(body_of(src, "void pop_back() noexcept")) ==
          "{\n"
          "        _M_bind();\n"
          "        __glibcxx_assert(!this->empty());\n"
          "        _End* __neu = _S_new_end();\n"
          "        _Node* __n = _M_c->tail;\n"
          "        _Node* __prev = __n->_M_prev;\n"
          "        if (__prev)\n"
          "            __prev->_M_next = nullptr;\n"
          "        else\n"
          "            _M_c->head = nullptr;\n"
          "        _M_c->tail = __prev;\n"
          "        --_M_c->size;\n"
          "        _S_delete_node(__n);\n"
          "        ::operator delete(_M_c->endc);\n"
          "        _M_c->endc = __neu;\n"
          "    }");
    CHECK(body_of(src, "iterator insert(const_iterator __pos, const value_type& __x)") ==
          "{ return emplace(__pos, __x); }");
    CHECK(body_of(src, "iterator insert(const_iterator __pos, value_type&& __x)") ==
          "{ return emplace(__pos, std::move(__x)); }");
    CHECK(norm_nl(body_of(src, "iterator insert(const_iterator __pos, size_type __n, const value_type& __x)")) ==
          "{\n"
          "        if (__n == 0) return _M_reify(__pos);\n"
          "        iterator __where = _M_reify(__pos);\n"
          "        iterator __first = __where;\n"
          "        for (size_type __i = 0; __i < __n; ++__i) {\n"
          "            __where = emplace(__where, __x);\n"
          "            if (__i == 0) __first = __where;\n"
          "            ++__where;\n"
          "        }\n"
          "        return __first;\n"
          "    }");
    CHECK(norm_nl(body_of(src, "iterator insert(const_iterator __pos, _InputIterator __first, _InputIterator __last)")) ==
          "{\n"
          "        if (__first == __last) return _M_reify(__pos);\n"
          "        iterator __where = _M_reify(__pos);\n"
          "        iterator __at = __where;\n"
          "        bool __have = false;\n"
          "        for (; __first != __last; ++__first) {\n"
          "            __where = emplace(__where, *__first);\n"
          "            if (!__have) {\n"
          "                __at = __where;\n"
          "                __have = true;\n"
          "            }\n"
          "            ++__where;\n"
          "        }\n"
          "        return __at;\n"
          "    }");
    CHECK(norm_nl(body_of(src, "iterator insert(const_iterator __pos, initializer_list<value_type> __l)")) ==
          "{\n"
          "        return insert(__pos, __l.begin(), __l.end());\n"
          "    }");
    CHECK(norm_nl(body_of(src, "iterator erase(const_iterator __first, const_iterator __last)")) ==
          "{\n"
          "        _M_bind();\n"
          "        __glibcxx_assert(__first._M_ctrl == _M_c && __last._M_ctrl == _M_c);\n"
          "        if (__first == __last) return _M_reify(__first);\n"
          "        __glibcxx_assert(!__first._M_is_end);\n"
          "        const bool __prefix = __first._M_node == _M_c->head;\n"
          "        const bool __suffix = __last._M_is_end;\n"
          "        const bool __middle = !__prefix && !__suffix;\n"
          "        // Allocate the replacement anchor/cookie before unlinking so a\n"
          "        // throwing operator new leaves the deque unchanged.\n"
          "        _Anchor* __neu_a = __middle ? _S_new_anchor() : nullptr;\n"
          "        _End* __neu_e = __suffix ? _S_new_end() : nullptr;\n"
          "        _Node* __stop = __last._M_is_end ? nullptr : __last._M_node;\n"
          "        _Node* __before = __first._M_node->_M_prev;\n"
          "        _Node* __cur = __first._M_node;\n"
          "        while (__cur != __stop) {\n"
          "            _Node* __next = __cur->_M_next;\n"
          "            _S_delete_node(__cur);\n"
          "            --_M_c->size;\n"
          "            __cur = __next;\n"
          "        }\n"
          "        if (__before)\n"
          "            __before->_M_next = __stop;\n"
          "        else\n"
          "            _M_c->head = __stop;\n"
          "        if (__stop)\n"
          "            __stop->_M_prev = __before;\n"
          "        else\n"
          "            _M_c->tail = __before;\n"
          "        if (__neu_a) {\n"
          "            _Anchor* __old = _M_c->anchor;\n"
          "            _M_c->anchor = __neu_a;\n"
          "            ::operator delete(__old);\n"
          "        }\n"
          "        if (__neu_e) {\n"
          "            _End* __old = _M_c->endc;\n"
          "            _M_c->endc = __neu_e;\n"
          "            ::operator delete(__old);\n"
          "        }\n"
          "        if (!__stop) return end();\n"
          "        return iterator(_M_c->anchor, __stop, false, nullptr, _M_c);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "iterator erase(const_iterator __pos)")) ==
          "{\n"
          "        __glibcxx_assert(__pos._M_ctrl == _M_c && !__pos._M_is_end);\n"
          "        const_iterator __next = __pos;\n"
          "        ++__next;\n"
          "        return erase(__pos, __next);\n"
          "    }");
    CHECK(norm_nl(body_of(src, "void clear() noexcept")) ==
          "{\n"
          "        if (!_M_c) return;\n"
          "        _Anchor* __neu = _S_new_anchor();\n"
          "        _End* __en = _S_new_end();\n"
          "        _M_destroy_elems();\n"
          "        ::operator delete(_M_c->anchor);\n"
          "        ::operator delete(_M_c->endc);\n"
          "        _M_c->anchor = __neu;\n"
          "        _M_c->endc = __en;\n"
          "    }");
    CHECK(body_of(src, "void swap(deque& __x) noexcept") == "{ std::swap(_M_c, __x._M_c); }");
    CHECK(norm_nl(body_of(src, "void swap(deque<_Tp, _Alloc>& __x, deque<_Tp, _Alloc>& __y) noexcept")) ==
          "{\n"
          "    __x.swap(__y);\n"
          "}");

    const std::string code = strip_comments_and_strings(src);
    CHECK(count_sub(code, "operator!=") == 1);
    CHECK(count_sub(code, ne_sig) == 1);
    CHECK(code.find("operator!=(const _Iter") == std::string::npos);
    CHECK(code.find("operator!=(const iterator") == std::string::npos);
    CHECK(code.find("operator!=(iterator") == std::string::npos);
    CHECK(code.find("operator!=(const const_iterator") == std::string::npos);
    CHECK(code.find("bool operator==(const _Iter<_Other>& __o) const noexcept") != std::string::npos);

    const std::string cmake = slurp(repo() / "CMakeLists.txt");
    CHECK(count_sub(cmake, "tests/cpp/test_cxx_deque_ne.cpp") == 1);
    CHECK(cmake.find("tests/cpp/test_cxx_deque.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_cxx_list_ne.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_cxx_vector_ne.cpp") != std::string::npos);
    CHECK(cmake.find("tests/cpp/test_cxx_forward_list_ne.cpp") != std::string::npos);
}
