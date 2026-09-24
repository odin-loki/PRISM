// PRISM operational model of the out-of-line members of std::basic_string
// (roadmap 2.6, docs/PIR.md "C++ library models"). libstdc++'s <string>
// includes <bits/basic_string.tcc> after the class definition
// (bits/basic_string.h, used as it is); the pir stage puts this directory
// first on the C++ include path, so that include reaches this file.
//
// Why a model: <string> is reached from every iostream/exception header, so
// the class itself cannot be swapped, but its out-of-line members (the
// constructors' _M_construct, _M_create, _M_mutate, _M_replace, _M_append,
// reserve, swap, find...) are where the solver time goes: libstdc++ copies
// with memcpy/memmove of a symbolic length, which PIR encodes as a ranged
// copy the solver handles slowly (cxx_string_substr timed out). Here every
// copy is a short element loop the encoder closes quickly, and everything
// else is libstdc++ 13's observable behaviour, member by member:
//
//  * the same storage policy: the 15-character local buffer, _M_create's
//    growth (a request below twice the old capacity gets twice the old
//    capacity, clamped to max_size()), the same capacity after every
//    operation, allocation through std::allocator (operator new/delete, PIR
//    models), so a pointer or iterator kept across a reallocation points into
//    a freed object (MEM-UAF);
//  * the same exceptions with the same messages (length_error from
//    _M_create/_M_check_length, out_of_range from _M_check);
//  * the same results for overlapping arguments taken from the string itself
//    (the in-place replace of _M_replace_cold), and the same operations that
//    are undefined behaviour: a read or write outside the objects is checked
//    by the memory model on every element, and resize_and_overwrite's result
//    above n is a precondition failure (libstdc++: __builtin_unreachable).
//
// The class body, its inline members and its _GLIBCXX_ASSERTIONS checks are
// libstdc++'s own. Only the new ABI (_GLIBCXX_USE_CXX11_ABI) is modelled; the
// old copy-on-write string gets libstdc++'s file. operator>> and getline
// are not defined here: for char and wchar_t they are explicit instantiations
// in libstdc++.so (extern templates), as they are with libstdc++'s file.
// tests/cxx_models/string_trace.cpp is compared against libstdc++ under
// ASan/UBSan (tests/test_cxx_models.py).
#ifndef _BASIC_STRING_TCC
#pragma GCC system_header

#if !_GLIBCXX_USE_CXX11_ABI
#include_next <bits/basic_string.tcc>
#else
#define _BASIC_STRING_TCC 1
#define _PRISM_MODEL_BASIC_STRING_TCC 1

#include <bits/cxxabi_forced.h>

namespace std _GLIBCXX_VISIBILITY(default) {
_GLIBCXX_BEGIN_NAMESPACE_VERSION

// Element loops (libstdc++: memcpy/memmove/memset through char_traits).
template <typename _CharT, typename _Traits>
_GLIBCXX20_CONSTEXPR inline void __prism_str_copy(_CharT* __d, const _CharT* __s, size_t __n) {
    for (size_t __i = 0; __i < __n; ++__i) _Traits::assign(__d[__i], __s[__i]);
}
// __d and __s point into the same buffer (memmove)
template <typename _CharT, typename _Traits>
_GLIBCXX20_CONSTEXPR inline void __prism_str_move(_CharT* __d, const _CharT* __s, size_t __n) {
    if (__d == __s || __n == 0) return;
    if (std::less<const _CharT*>()(__d, __s)) {
        for (size_t __i = 0; __i < __n; ++__i) _Traits::assign(__d[__i], __s[__i]);
    } else {
        for (size_t __i = __n; __i > 0; --__i) _Traits::assign(__d[__i - 1], __s[__i - 1]);
    }
}
template <typename _CharT, typename _Traits>
_GLIBCXX20_CONSTEXPR inline void __prism_str_fill(_CharT* __d, size_t __n, _CharT __c) {
    for (size_t __i = 0; __i < __n; ++__i) _Traits::assign(__d[__i], __c);
}
// is __c one of __s[0..__n)
template <typename _CharT, typename _Traits>
_GLIBCXX20_CONSTEXPR inline bool __prism_str_has(const _CharT* __s, size_t __n, _CharT __c) {
    for (size_t __i = 0; __i < __n; ++__i)
        if (_Traits::eq(__s[__i], __c)) return true;
    return false;
}
// do __a[0..__n) and __b[0..__n) hold the same characters
template <typename _CharT, typename _Traits>
_GLIBCXX20_CONSTEXPR inline bool __prism_str_same(const _CharT* __a, const _CharT* __b, size_t __n) {
    for (size_t __i = 0; __i < __n; ++__i)
        if (!_Traits::eq(__a[__i], __b[__i])) return false;
    return true;
}

template <typename _CharT, typename _Traits, typename _Alloc>
const typename basic_string<_CharT, _Traits, _Alloc>::size_type basic_string<_CharT, _Traits, _Alloc>::npos;

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR void basic_string<_CharT, _Traits, _Alloc>::swap(basic_string& __s) _GLIBCXX_NOEXCEPT {
    if (this == std::__addressof(__s)) return;
    _Alloc_traits::_S_on_swap(_M_get_allocator(), __s._M_get_allocator());
    const size_type __len = length(), __slen = __s.length();
    if (_M_is_local() && __s._M_is_local()) {
        // both in their local buffers: exchange the characters (and the NULs)
        _CharT __tmp[_S_local_capacity + 1];
        __prism_str_copy<_CharT, _Traits>(__tmp, __s._M_local_buf, __slen + 1);
        __s._M_init_local_buf();
        __prism_str_copy<_CharT, _Traits>(__s._M_local_buf, _M_local_buf, __len + 1);
        _M_init_local_buf();
        __prism_str_copy<_CharT, _Traits>(_M_local_buf, __tmp, __slen + 1);
    } else if (_M_is_local()) {
        // __s's heap buffer comes here; our characters go to its local buffer
        const size_type __cap = __s._M_allocated_capacity;
        pointer __heap = __s._M_data();
        __s._M_init_local_buf();
        __prism_str_copy<_CharT, _Traits>(__s._M_local_buf, _M_local_buf, __len + 1);
        __s._M_data(__s._M_local_data());
        _M_data(__heap);
        _M_capacity(__cap);
    } else if (__s._M_is_local()) {
        const size_type __cap = _M_allocated_capacity;
        pointer __heap = _M_data();
        _M_init_local_buf();
        __prism_str_copy<_CharT, _Traits>(_M_local_buf, __s._M_local_buf, __slen + 1);
        _M_data(_M_local_data());
        __s._M_data(__heap);
        __s._M_capacity(__cap);
    } else {
        const size_type __cap = _M_allocated_capacity;
        pointer __heap = _M_data();
        _M_data(__s._M_data());
        _M_capacity(__s._M_allocated_capacity);
        __s._M_data(__heap);
        __s._M_capacity(__cap);
    }
    _M_length(__slen);
    __s._M_length(__len);
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::pointer
basic_string<_CharT, _Traits, _Alloc>::_M_create(size_type& __capacity, size_type __old_capacity) {
    if (__capacity > max_size()) std::__throw_length_error(__N("basic_string::_M_create"));
    // exponential growth: a request between the old capacity and twice it
    // gets twice it (never more than max_size())
    if (__capacity > __old_capacity && __capacity < 2 * __old_capacity)
        __capacity = (std::min)(size_type(2 * __old_capacity), max_size());
    return _S_allocate(_M_get_allocator(), __capacity + 1);  // + the terminator
}

template <typename _CharT, typename _Traits, typename _Alloc>
template <typename _InIterator>
_GLIBCXX20_CONSTEXPR void basic_string<_CharT, _Traits, _Alloc>::_M_construct(_InIterator __beg, _InIterator __end,
                                                                               std::input_iterator_tag) {
    // single pass: fill the local buffer, then grow one character at a time
    // (each growth through _M_create's policy)
    size_type __len = 0;
    size_type __cap = size_type(_S_local_capacity);
    _M_init_local_buf();
    for (; __beg != __end && __len < __cap; ++__beg) _M_local_buf[__len++] = *__beg;
    try {
        for (; __beg != __end; ++__beg) {
            if (__len == __cap) {
                __cap = __len + 1;
                pointer __grown = _M_create(__cap, __len);
                __prism_str_copy<_CharT, _Traits>(__grown, _M_data(), __len);
                _M_dispose();
                _M_data(__grown);
                _M_capacity(__cap);
            }
            _Traits::assign(_M_data()[__len++], *__beg);
        }
    } catch (...) {
        _M_dispose();
        throw;
    }
    _M_set_length(__len);
}

template <typename _CharT, typename _Traits, typename _Alloc>
template <typename _FwdIterator>
_GLIBCXX20_CONSTEXPR void basic_string<_CharT, _Traits, _Alloc>::_M_construct(_FwdIterator __beg, _FwdIterator __end,
                                                                               std::forward_iterator_tag) {
    size_type __n = static_cast<size_type>(std::distance(__beg, __end));
    if (__n > size_type(_S_local_capacity)) {
        _M_data(_M_create(__n, size_type(0)));
        _M_capacity(__n);
    } else {
        _M_init_local_buf();
    }
    if constexpr (is_pointer<_FwdIterator>::value) {
        __prism_str_copy<_CharT, _Traits>(_M_data(), __beg, __n);
    } else if constexpr (noexcept(++__beg) && noexcept(*__beg) && noexcept(__beg != __end)) {
        pointer __p = _M_data();
        for (; __beg != __end; ++__beg, (void)++__p) _Traits::assign(*__p, *__beg);
    } else {
        // the iterator's operations may throw: release the buffer then
        try {
            pointer __p = _M_data();
            for (; __beg != __end; ++__beg, (void)++__p) _Traits::assign(*__p, *__beg);
        } catch (...) {
            _M_dispose();
            throw;
        }
    }
    _M_set_length(__n);
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR void basic_string<_CharT, _Traits, _Alloc>::_M_construct(size_type __n, _CharT __c) {
    if (__n > size_type(_S_local_capacity)) {
        _M_data(_M_create(__n, size_type(0)));
        _M_capacity(__n);
    } else {
        _M_init_local_buf();
    }
    __prism_str_fill<_CharT, _Traits>(_M_data(), __n, __c);
    _M_set_length(__n);
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR void basic_string<_CharT, _Traits, _Alloc>::_M_assign(const basic_string& __str) {
    if (this == std::__addressof(__str)) return;
    const size_type __n = __str.length();
    const size_type __cap = capacity();
    if (__n > __cap) {
        size_type __new_cap = __n;
        pointer __p = _M_create(__new_cap, __cap);  // may throw: nothing changed yet
        _M_dispose();
        _M_data(__p);
        _M_capacity(__new_cap);
    }
    __prism_str_copy<_CharT, _Traits>(_M_data(), __str._M_data(), __n);
    _M_set_length(__n);
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR void basic_string<_CharT, _Traits, _Alloc>::reserve(size_type __res) {
    const size_type __cap = capacity();
    if (__res <= __cap) return;  // never shrinks (P0966)
    pointer __p = _M_create(__res, __cap);
    __prism_str_copy<_CharT, _Traits>(__p, _M_data(), length() + 1);
    _M_dispose();
    _M_data(__p);
    _M_capacity(__res);
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR void basic_string<_CharT, _Traits, _Alloc>::_M_mutate(size_type __pos, size_type __len1,
                                                                            const _CharT* __s, size_type __len2) {
    // new storage holding [0, pos) + s[0, len2) + the tail after pos + len1;
    // the caller sets the length (and the terminator)
    const size_type __tail = length() - __pos - __len1;
    size_type __new_cap = length() + __len2 - __len1;
    pointer __p = _M_create(__new_cap, capacity());
    __prism_str_copy<_CharT, _Traits>(__p, _M_data(), __pos);
    if (__s) __prism_str_copy<_CharT, _Traits>(__p + __pos, __s, __len2);
    __prism_str_copy<_CharT, _Traits>(__p + __pos + __len2, _M_data() + __pos + __len1, __tail);
    _M_dispose();
    _M_data(__p);
    _M_capacity(__new_cap);
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR void basic_string<_CharT, _Traits, _Alloc>::_M_erase(size_type __pos, size_type __n) {
    const size_type __tail = length() - __pos - __n;
    if (__tail && __n) __prism_str_move<_CharT, _Traits>(_M_data() + __pos, _M_data() + __pos + __n, __tail);
    _M_set_length(length() - __n);
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR void basic_string<_CharT, _Traits, _Alloc>::reserve() {
    // non-binding shrink request (C++20 reserve() / shrink_to_fit)
    if (_M_is_local()) return;
    const size_type __len = length();
    const size_type __cap = _M_allocated_capacity;
    if (__len <= size_type(_S_local_capacity)) {
        _M_init_local_buf();
        __prism_str_copy<_CharT, _Traits>(_M_local_buf, _M_data(), __len + 1);
        _M_destroy(__cap);
        _M_data(_M_local_data());
    } else if (__len < __cap) {
        try {
            pointer __p = _S_allocate(_M_get_allocator(), __len + 1);
            __prism_str_copy<_CharT, _Traits>(__p, _M_data(), __len + 1);
            _M_dispose();
            _M_data(__p);
            _M_capacity(__len);
        } catch (const __cxxabiv1::__forced_unwind&) {
            throw;
        } catch (...) {
            // an allocation failure keeps the string as it is
        }
    }
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR void basic_string<_CharT, _Traits, _Alloc>::resize(size_type __n, _CharT __c) {
    const size_type __size = this->size();
    if (__size < __n)
        this->append(__n - __size, __c);
    else if (__n < __size)
        this->_M_set_length(__n);
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR basic_string<_CharT, _Traits, _Alloc>& basic_string<_CharT, _Traits, _Alloc>::_M_append(
    const _CharT* __s, size_type __n) {
    const size_type __size = this->size();
    const size_type __len = __n + __size;
    if (__len <= this->capacity())
        __prism_str_copy<_CharT, _Traits>(this->_M_data() + __size, __s, __n);
    else
        this->_M_mutate(__size, size_type(0), __s, __n);
    this->_M_set_length(__len);
    return *this;
}

template <typename _CharT, typename _Traits, typename _Alloc>
template <typename _InputIterator>
_GLIBCXX20_CONSTEXPR basic_string<_CharT, _Traits, _Alloc>& basic_string<_CharT, _Traits, _Alloc>::_M_replace_dispatch(
    const_iterator __i1, const_iterator __i2, _InputIterator __k1, _InputIterator __k2, std::__false_type) {
    // the new characters are read first (the range may alias this string)
    const basic_string __s(__k1, __k2, this->get_allocator());
    const size_type __n1 = __i2 - __i1;
    return _M_replace(__i1 - begin(), __n1, __s._M_data(), __s.size());
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR basic_string<_CharT, _Traits, _Alloc>& basic_string<_CharT, _Traits, _Alloc>::_M_replace_aux(
    size_type __pos1, size_type __n1, size_type __n2, _CharT __c) {
    _M_check_length(__n1, __n2, "basic_string::_M_replace_aux");
    const size_type __old_size = this->size();
    const size_type __new_size = __old_size + __n2 - __n1;
    if (__new_size <= this->capacity()) {
        pointer __p = this->_M_data() + __pos1;
        const size_type __tail = __old_size - __pos1 - __n1;
        if (__tail && __n1 != __n2) __prism_str_move<_CharT, _Traits>(__p + __n2, __p + __n1, __tail);
    } else {
        this->_M_mutate(__pos1, __n1, 0, __n2);
    }
    __prism_str_fill<_CharT, _Traits>(this->_M_data() + __pos1, __n2, __c);
    this->_M_set_length(__new_size);
    return *this;
}

// Replace [p, p + len1) by s[0, len2) in place when s points into this string.
template <typename _CharT, typename _Traits, typename _Alloc>
__attribute__((__noinline__, __noclone__, __cold__)) void basic_string<_CharT, _Traits, _Alloc>::_M_replace_cold(
    pointer __p, size_type __len1, const _CharT* __s, const size_type __len2, const size_type __how_much) {
    if (__len2 <= __len1) {
        // shrinking or same size: the source moves first, then the tail
        __prism_str_move<_CharT, _Traits>(__p, __s, __len2);
        if (__how_much && __len1 != __len2) __prism_str_move<_CharT, _Traits>(__p + __len2, __p + __len1, __how_much);
        return;
    }
    // growing: the tail moves up first; the part of the source that was in
    // the tail moved with it by len2 - len1
    if (__how_much) __prism_str_move<_CharT, _Traits>(__p + __len2, __p + __len1, __how_much);
    if (__s + __len2 <= __p + __len1) {
        __prism_str_move<_CharT, _Traits>(__p, __s, __len2);
    } else if (__s >= __p + __len1) {
        __prism_str_copy<_CharT, _Traits>(__p, __s + (__len2 - __len1), __len2);
    } else {
        const size_type __before = (__p + __len1) - __s;  // source characters left of the tail
        __prism_str_move<_CharT, _Traits>(__p, __s, __before);
        __prism_str_copy<_CharT, _Traits>(__p + __before, __p + __len2, __len2 - __before);
    }
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR basic_string<_CharT, _Traits, _Alloc>& basic_string<_CharT, _Traits, _Alloc>::_M_replace(
    size_type __pos, size_type __len1, const _CharT* __s, const size_type __len2) {
    _M_check_length(__len1, __len2, "basic_string::_M_replace");
    const size_type __old_size = this->size();
    const size_type __new_size = __old_size + __len2 - __len1;
    if (__new_size <= this->capacity()) {
        pointer __p = this->_M_data() + __pos;
        const size_type __tail = __old_size - __pos - __len1;
        if (std::__is_constant_evaluated()) {
            // constant evaluation cannot order unrelated pointers: go through a copy
            pointer __q = _S_allocate(_M_get_allocator(), __new_size);
            __prism_str_copy<_CharT, _Traits>(__q, this->_M_data(), __pos);
            __prism_str_copy<_CharT, _Traits>(__q + __pos, __s, __len2);
            __prism_str_copy<_CharT, _Traits>(__q + __pos + __len2, __p + __len1, __tail);
            __prism_str_copy<_CharT, _Traits>(this->_M_data(), __q, __new_size);
            this->_M_get_allocator().deallocate(__q, __new_size);
        } else if (_M_disjunct(__s)) {
            if (__tail && __len1 != __len2) __prism_str_move<_CharT, _Traits>(__p + __len2, __p + __len1, __tail);
            __prism_str_copy<_CharT, _Traits>(__p, __s, __len2);
        } else {
            _M_replace_cold(__p, __len1, __s, __len2, __tail);
        }
    } else {
        this->_M_mutate(__pos, __len1, __s, __len2);
    }
    this->_M_set_length(__new_size);
    return *this;
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::size_type
basic_string<_CharT, _Traits, _Alloc>::copy(_CharT* __s, size_type __n, size_type __pos) const {
    _M_check(__pos, "basic_string::copy");
    __n = _M_limit(__pos, __n);
    __prism_str_copy<_CharT, _Traits>(__s, _M_data() + __pos, __n);
    return __n;
}

#if __cplusplus > 202002L
template <typename _CharT, typename _Traits, typename _Alloc>
template <typename _Operation>
constexpr void basic_string<_CharT, _Traits, _Alloc>::resize_and_overwrite(const size_type __n, _Operation __op) {
    const size_type __cap = capacity();
    _CharT* __p;
    if (__n > __cap) {
        size_type __new_cap = __n;
        __p = _M_create(__new_cap, __cap);
        __prism_str_copy<_CharT, _Traits>(__p, _M_data(), length());  // not the terminator
        if (std::is_constant_evaluated()) __prism_str_fill<_CharT, _Traits>(__p + length(), __n - length(), _CharT());
        _M_dispose();
        _M_data(__p);
        _M_capacity(__new_cap);
    } else {
        __p = _M_data();
    }
    size_type __r = 0;
    try {
        auto __res = std::move(__op)(auto(__p), auto(__n));
        static_assert(ranges::__detail::__is_integer_like<decltype(__res)>);
        // libstdc++: a result outside [0, n] is __builtin_unreachable()
        __glibcxx_assert(__res >= 0 && size_type(__res) <= __n);
        __r = size_type(__res);
    } catch (...) {
        _M_set_length(0);
        throw;
    }
    _M_set_length(__r);
}
#endif

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::size_type
basic_string<_CharT, _Traits, _Alloc>::find(const _CharT* __s, size_type __pos, size_type __n) const _GLIBCXX_NOEXCEPT {
    const size_type __size = this->size();
    if (__n == 0) return __pos <= __size ? __pos : npos;
    if (__pos >= __size || __n > __size - __pos) return npos;
    const _CharT* __data = _M_data();
    for (size_type __i = __pos; __i <= __size - __n; ++__i)
        if (__prism_str_same<_CharT, _Traits>(__data + __i, __s, __n)) return __i;
    return npos;
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::size_type
basic_string<_CharT, _Traits, _Alloc>::find(_CharT __c, size_type __pos) const _GLIBCXX_NOEXCEPT {
    const size_type __size = this->size();
    for (; __pos < __size; ++__pos)
        if (_Traits::eq(_M_data()[__pos], __c)) return __pos;
    return npos;
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::size_type
basic_string<_CharT, _Traits, _Alloc>::rfind(const _CharT* __s, size_type __pos, size_type __n) const
    _GLIBCXX_NOEXCEPT {
    const size_type __size = this->size();
    if (__n > __size) return npos;
    size_type __i = (std::min)(size_type(__size - __n), __pos);
    for (;; --__i) {
        if (__prism_str_same<_CharT, _Traits>(_M_data() + __i, __s, __n)) return __i;
        if (__i == 0) return npos;
    }
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::size_type
basic_string<_CharT, _Traits, _Alloc>::rfind(_CharT __c, size_type __pos) const _GLIBCXX_NOEXCEPT {
    const size_type __size = this->size();
    if (__size == 0) return npos;
    size_type __i = (std::min)(size_type(__size - 1), __pos);
    for (;; --__i) {
        if (_Traits::eq(_M_data()[__i], __c)) return __i;
        if (__i == 0) return npos;
    }
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::size_type
basic_string<_CharT, _Traits, _Alloc>::find_first_of(const _CharT* __s, size_type __pos, size_type __n) const
    _GLIBCXX_NOEXCEPT {
    if (__n == 0) return npos;
    for (; __pos < this->size(); ++__pos)
        if (__prism_str_has<_CharT, _Traits>(__s, __n, _M_data()[__pos])) return __pos;
    return npos;
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::size_type
basic_string<_CharT, _Traits, _Alloc>::find_last_of(const _CharT* __s, size_type __pos, size_type __n) const
    _GLIBCXX_NOEXCEPT {
    const size_type __size = this->size();
    if (__size == 0 || __n == 0) return npos;
    size_type __i = (std::min)(size_type(__size - 1), __pos);
    for (;; --__i) {
        if (__prism_str_has<_CharT, _Traits>(__s, __n, _M_data()[__i])) return __i;
        if (__i == 0) return npos;
    }
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::size_type
basic_string<_CharT, _Traits, _Alloc>::find_first_not_of(const _CharT* __s, size_type __pos, size_type __n) const
    _GLIBCXX_NOEXCEPT {
    for (; __pos < this->size(); ++__pos)
        if (!__prism_str_has<_CharT, _Traits>(__s, __n, _M_data()[__pos])) return __pos;
    return npos;
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::size_type
basic_string<_CharT, _Traits, _Alloc>::find_first_not_of(_CharT __c, size_type __pos) const _GLIBCXX_NOEXCEPT {
    for (; __pos < this->size(); ++__pos)
        if (!_Traits::eq(_M_data()[__pos], __c)) return __pos;
    return npos;
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::size_type
basic_string<_CharT, _Traits, _Alloc>::find_last_not_of(const _CharT* __s, size_type __pos, size_type __n) const
    _GLIBCXX_NOEXCEPT {
    const size_type __size = this->size();
    if (__size == 0) return npos;
    size_type __i = (std::min)(size_type(__size - 1), __pos);
    for (;; --__i) {
        if (!__prism_str_has<_CharT, _Traits>(__s, __n, _M_data()[__i])) return __i;
        if (__i == 0) return npos;
    }
}

template <typename _CharT, typename _Traits, typename _Alloc>
_GLIBCXX20_CONSTEXPR typename basic_string<_CharT, _Traits, _Alloc>::size_type
basic_string<_CharT, _Traits, _Alloc>::find_last_not_of(_CharT __c, size_type __pos) const _GLIBCXX_NOEXCEPT {
    const size_type __size = this->size();
    if (__size == 0) return npos;
    size_type __i = (std::min)(size_type(__size - 1), __pos);
    for (;; --__i) {
        if (!_Traits::eq(_M_data()[__i], __c)) return __i;
        if (__i == 0) return npos;
    }
}

_GLIBCXX_END_NAMESPACE_VERSION
}  // namespace std

#endif  // _GLIBCXX_USE_CXX11_ABI
#endif  // _BASIC_STRING_TCC
