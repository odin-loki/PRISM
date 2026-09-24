// PRISM operational model: the ordered node container behind <map> and
// <set> (roadmap 2.6, docs/PIR.md "C++ library models"). Not a public
// header: `map` and `set` in this directory include it.
//
// libstdc++'s red-black tree (bits/stl_tree.h) erases and copies recursively,
// which the PIR encoder does not inline (`UNENCODED: recursive call`), so
// every std::map/std::set function was NEEDS-HARNESS. This model keeps the
// elements in a sorted, circular, doubly linked list of heap nodes around a
// sentinel header (the end() position) that lives in the container object.
// Every observable behaviour the standard gives the associative containers
// is the same as libstdc++ 13's:
//
//  * the element order (key_compare), uniqueness (map/set), and where an
//    equivalent key goes in a multimap/multiset: insert/emplace after the
//    equivalent keys, a hinted insert as close as possible before the hint
//    with libstdc++'s exact rule (_M_get_insert_hint_equal_pos);
//  * one heap node per element from std::allocator (operator new/delete, PIR
//    models): insertion never invalidates, erase frees exactly the erased
//    node, so an iterator, pointer or reference to an erased element points
//    into a freed object and using it is MEM-UAF, as it is undefined
//    behaviour with libstdc++; clear(), the destructor and assignment free
//    every node of the target;
//  * when an element is constructed: emplace/emplace_hint/insert(P&&) build
//    the node first and drop it if the key exists (libstdc++'s
//    _M_emplace_unique), insert(value_type)/try_emplace/operator[]/
//    insert_or_assign look up first and construct only when the key is
//    absent; copies construct every element once; a throwing element
//    constructor leaves the container unchanged (copy assignment leaves the
//    target empty, as libstdc++'s node-reusing assignment does);
//  * max_size() is allocator_traits<allocator<node>>::max_size() (C++20:
//    SIZE_MAX / sizeof(node)) for the size of libstdc++'s node;
//  * the same exceptions (map::at: out_of_range) and preconditions, checked
//    as _GLIBCXX_ASSERTIONS checks (FUNC-CONTRACT): dereferencing or
//    incrementing end(), decrementing begin(), erase(end()), and an iterator
//    or hint that belongs to another container.
//
// What it does not model is not silently different: node handles (extract,
// insert(node_type), merge) and allocators other than std::allocator are left
// undefined, so a unit that uses them does not compile against the model and
// the pir stage lowers it again with libstdc++ (reported in the function's
// `extra.cxx_models`). The number and order of comparator calls and of
// element destructions inside clear()/the destructor are unspecified by the
// standard and differ from libstdc++'s tree.
#ifndef _PRISM_MODEL_TREE_H
#define _PRISM_MODEL_TREE_H 1

#pragma GCC system_header

#include <bits/c++config.h>
#include <bits/stl_algobase.h>
#include <bits/allocator.h>
#include <bits/alloc_traits.h>
#include <bits/stl_function.h>
#include <bits/stl_pair.h>
#include <bits/stl_iterator_base_funcs.h>
#include <bits/stl_iterator.h>
#include <bits/functexcept.h>
#include <bits/range_access.h>
#include <bits/stl_algo.h>
#include <ext/aligned_buffer.h>
#include <initializer_list>
#include <type_traits>
#include <compare>
#include <tuple>
#include <bits/memory_resource.h>
#include <debug/assertions.h>

#define __cpp_lib_generic_associative_lookup 201304L
#define __cpp_lib_erase_if 202002L
#define __cpp_lib_nonmember_container_access 201411L
#define __cpp_lib_allocator_traits_is_always_equal 201411L

namespace std _GLIBCXX_VISIBILITY(default) {

struct _Prism_node_base {
    _Prism_node_base* _M_next;
    _Prism_node_base* _M_prev;
    // the header of the container that owns the node (the header owns itself)
    const _Prism_node_base* _M_owner;

    // a precondition check (FUNC-CONTRACT): not the end() position
    bool _M_dereferenceable() const noexcept { return _M_owner != this; }
};

template <typename _Val>
struct _Prism_node : _Prism_node_base {
    __gnu_cxx::__aligned_membuf<_Val> _M_storage;
    _Val* _M_valptr() noexcept { return _M_storage._M_ptr(); }
    const _Val* _M_valptr() const noexcept { return _M_storage._M_ptr(); }
};

// The layout of libstdc++'s _Rb_tree_node<_Val>: max_size() is computed from it.
template <typename _Val>
struct _Prism_rb_node_layout {
    int _M_color;
    void* _M_links[3];
    __gnu_cxx::__aligned_membuf<_Val> _M_storage;
};

template <typename _Val>
struct _Prism_tree_const_iterator;

template <typename _Val>
struct _Prism_tree_iterator {
    typedef _Val value_type;
    typedef _Val& reference;
    typedef _Val* pointer;
    typedef bidirectional_iterator_tag iterator_category;
    typedef ptrdiff_t difference_type;

    _Prism_node_base* _M_node = nullptr;

    _Prism_tree_iterator() = default;
    explicit _Prism_tree_iterator(_Prism_node_base* __x) noexcept : _M_node(__x) {}

    reference operator*() const noexcept {
        __glibcxx_assert(_M_node->_M_dereferenceable());
        return *static_cast<_Prism_node<_Val>*>(_M_node)->_M_valptr();
    }
    pointer operator->() const noexcept {
        __glibcxx_assert(_M_node->_M_dereferenceable());
        return static_cast<_Prism_node<_Val>*>(_M_node)->_M_valptr();
    }
    _Prism_tree_iterator& operator++() noexcept {
        __glibcxx_assert(_M_node->_M_dereferenceable());
        _M_node = _M_node->_M_next;
        return *this;
    }
    _Prism_tree_iterator operator++(int) noexcept {
        _Prism_tree_iterator __tmp = *this;
        ++*this;
        return __tmp;
    }
    _Prism_tree_iterator& operator--() noexcept {
        _M_node = _M_node->_M_prev;
        __glibcxx_assert(_M_node->_M_dereferenceable());  // was not begin()
        return *this;
    }
    _Prism_tree_iterator operator--(int) noexcept {
        _Prism_tree_iterator __tmp = *this;
        --*this;
        return __tmp;
    }
    friend bool operator==(const _Prism_tree_iterator& __x, const _Prism_tree_iterator& __y) noexcept {
        return __x._M_node == __y._M_node;
    }
};

template <typename _Val>
struct _Prism_tree_const_iterator {
    typedef _Val value_type;
    typedef const _Val& reference;
    typedef const _Val* pointer;
    typedef _Prism_tree_iterator<_Val> iterator;
    typedef bidirectional_iterator_tag iterator_category;
    typedef ptrdiff_t difference_type;

    const _Prism_node_base* _M_node = nullptr;

    _Prism_tree_const_iterator() = default;
    explicit _Prism_tree_const_iterator(const _Prism_node_base* __x) noexcept : _M_node(__x) {}
    _Prism_tree_const_iterator(const iterator& __it) noexcept : _M_node(__it._M_node) {}

    iterator _M_const_cast() const noexcept { return iterator(const_cast<_Prism_node_base*>(_M_node)); }

    reference operator*() const noexcept {
        __glibcxx_assert(_M_node->_M_dereferenceable());
        return *static_cast<const _Prism_node<_Val>*>(_M_node)->_M_valptr();
    }
    pointer operator->() const noexcept {
        __glibcxx_assert(_M_node->_M_dereferenceable());
        return static_cast<const _Prism_node<_Val>*>(_M_node)->_M_valptr();
    }
    _Prism_tree_const_iterator& operator++() noexcept {
        __glibcxx_assert(_M_node->_M_dereferenceable());
        _M_node = _M_node->_M_next;
        return *this;
    }
    _Prism_tree_const_iterator operator++(int) noexcept {
        _Prism_tree_const_iterator __tmp = *this;
        ++*this;
        return __tmp;
    }
    _Prism_tree_const_iterator& operator--() noexcept {
        _M_node = _M_node->_M_prev;
        __glibcxx_assert(_M_node->_M_dereferenceable());  // was not begin()
        return *this;
    }
    _Prism_tree_const_iterator operator--(int) noexcept {
        _Prism_tree_const_iterator __tmp = *this;
        --*this;
        return __tmp;
    }
    friend bool operator==(const _Prism_tree_const_iterator& __x, const _Prism_tree_const_iterator& __y) noexcept {
        return __x._M_node == __y._M_node;
    }
};

// Key extraction without a functor object: _S_get is static.
template <typename _Val>
struct _Prism_identity {
    static const _Val& _S_get(const _Val& __x) noexcept { return __x; }
};

// The container core shared by map, multimap, set and multiset.
template <typename _Key, typename _Val, typename _KeyOfValue, typename _Compare, typename _Alloc, bool _Multi>
class _Prism_tree {
    static_assert(is_same_v<_Alloc, allocator<_Val>>,
                  "PRISM map/set model: only std::allocator is modelled (the pir stage falls back to libstdc++)");

public:
    typedef _Prism_node_base _Base;
    typedef _Prism_node<_Val> _Node;
    typedef allocator<_Node> _Node_alloc;
    typedef allocator_traits<_Node_alloc> _Node_traits;
    typedef _Prism_tree_iterator<_Val> iterator;
    typedef _Prism_tree_const_iterator<_Val> const_iterator;
    typedef size_t size_type;

    [[no_unique_address]] _Compare _M_cmp;
    [[no_unique_address]] _Node_alloc _M_na;
    size_type _M_count = 0;
    _Base _M_head;

    // comparisons that cannot throw need no clean-up path
    static constexpr bool _S_nothrow_cmp =
        noexcept(std::declval<const _Compare&>()(std::declval<const _Key&>(), std::declval<const _Key&>())) ||
        ((is_same_v<_Compare, less<_Key>> || is_same_v<_Compare, greater<_Key>> || is_same_v<_Compare, less<>> ||
          is_same_v<_Compare, greater<>>) &&
         is_scalar_v<_Key>);

    _Prism_tree() : _M_cmp() { _M_reset(); }
    explicit _Prism_tree(const _Compare& __c) : _M_cmp(__c) { _M_reset(); }
    _Prism_tree(const _Prism_tree& __x) : _M_cmp(__x._M_cmp) {
        _M_reset();
        _M_copy_from(__x);
    }
    _Prism_tree(_Prism_tree&& __x) noexcept(is_nothrow_move_constructible_v<_Compare>) : _M_cmp(std::move(__x._M_cmp)) {
        _M_reset();
        _M_steal(__x);
    }
    ~_Prism_tree() { _M_clear(); }

    _Prism_tree& operator=(const _Prism_tree& __x) {
        if (this != std::__addressof(__x)) {
            _M_clear();
            _M_cmp = __x._M_cmp;
            _M_copy_from(__x);
        }
        return *this;
    }
    _Prism_tree& operator=(_Prism_tree&& __x) noexcept(is_nothrow_move_assignable_v<_Compare>) {
        _M_clear();
        _M_cmp = std::move(__x._M_cmp);
        _M_steal(__x);
        return *this;
    }

    // -- nodes ---------------------------------------------------------------
    void _M_reset() noexcept {
        _M_head._M_next = _M_head._M_prev = &_M_head;
        _M_head._M_owner = &_M_head;
        _M_count = 0;
    }
    _Base* _M_end() noexcept { return &_M_head; }
    const _Base* _M_end() const noexcept { return &_M_head; }
    // (The code below avoids temporaries whose address is taken -- functor
    // and allocator objects, std::pair results, lambdas: at -O0 each is a
    // stack object the PIR memory model tracks.)
    static const _Key& _S_key(const _Base* __n) noexcept {
        return _KeyOfValue::_S_get(*static_cast<const _Node*>(__n)->_M_valptr());
    }
    // is __p an element with a key equivalent to __k (__p from _M_lower(__k))
    template <typename _Kt>
    bool _M_equiv(const _Base* __p, const _Kt& __k) const {
        return __p != &_M_head && !_M_cmp(__k, _S_key(__p));
    }

    // std::allocator<_Node> for the node, construct_at/destroy_at for the
    // value (what std::allocator's construct/destroy do)
    template <typename... _Args>
    _Node* _M_create(_Args&&... __args) {
        _Node* __n = _Node_traits::allocate(_M_na, 1);
        ::new (static_cast<void*>(__n)) _Node;
        if constexpr (is_nothrow_constructible_v<_Val, _Args&&...>) {
            std::construct_at(__n->_M_valptr(), std::forward<_Args>(__args)...);
        } else {
            try {
                std::construct_at(__n->_M_valptr(), std::forward<_Args>(__args)...);
            } catch (...) {
                _Node_traits::deallocate(_M_na, __n, 1);
                throw;
            }
        }
        return __n;
    }
    void _M_drop(_Base* __b) noexcept {
        _Node* __n = static_cast<_Node*>(__b);
        std::destroy_at(__n->_M_valptr());
        _Node_traits::deallocate(_M_na, __n, 1);
    }
    // The position a new node's key goes to (_M_lower / _M_upper /
    // _M_pos_hint_equal), freeing the node if a comparison throws.
    enum { _S_at_lower, _S_at_upper };
    template <int _Where>
    _Base* _M_pos_or_drop(_Node* __n) {
        if constexpr (_S_nothrow_cmp) {
            return _Where == _S_at_lower ? _M_lower(_S_key(__n)) : _M_upper(_S_key(__n));
        } else {
            try {
                return _Where == _S_at_lower ? _M_lower(_S_key(__n)) : _M_upper(_S_key(__n));
            } catch (...) {
                _M_drop(__n);
                throw;
            }
        }
    }
    bool _M_equiv_or_drop(_Base* __p, _Node* __n) {
        if constexpr (_S_nothrow_cmp) {
            return _M_equiv(__p, _S_key(__n));
        } else {
            try {
                return _M_equiv(__p, _S_key(__n));
            } catch (...) {
                _M_drop(__n);
                throw;
            }
        }
    }
    _Base* _M_hint_pos_or_drop(_Base* __h, _Node* __n) {
        if constexpr (_S_nothrow_cmp) {
            return _M_pos_hint_equal(__h, _S_key(__n));
        } else {
            try {
                return _M_pos_hint_equal(__h, _S_key(__n));
            } catch (...) {
                _M_drop(__n);
                throw;
            }
        }
    }
    // Links __n just before __pos.
    _Base* _M_link(_Base* __pos, _Base* __n) noexcept {
        __n->_M_next = __pos;
        __n->_M_prev = __pos->_M_prev;
        __pos->_M_prev->_M_next = __n;
        __pos->_M_prev = __n;
        __n->_M_owner = &_M_head;
        ++_M_count;
        return __n;
    }
    void _M_unlink(_Base* __n) noexcept {
        __n->_M_prev->_M_next = __n->_M_next;
        __n->_M_next->_M_prev = __n->_M_prev;
        --_M_count;
    }
    void _M_clear() noexcept {
        _Base* __x = _M_head._M_next;
        while (__x != &_M_head) {
            _Base* __y = __x->_M_next;
            _M_drop(__x);
            __x = __y;
        }
        _M_reset();
    }
    // Appends copies of __x's elements (all or nothing).
    void _M_copy_from(const _Prism_tree& __x) {
        const _Base* __s = __x._M_head._M_next;
        if constexpr (is_nothrow_copy_constructible_v<_Val>) {
            for (; __s != &__x._M_head; __s = __s->_M_next)
                _M_link(&_M_head, _M_create(*static_cast<const _Node*>(__s)->_M_valptr()));
        } else {
            try {
                for (; __s != &__x._M_head; __s = __s->_M_next)
                    _M_link(&_M_head, _M_create(*static_cast<const _Node*>(__s)->_M_valptr()));
            } catch (...) {
                _M_clear();
                throw;
            }
        }
    }
    // Takes every node of __x (which becomes empty); iterators stay valid.
    void _M_steal(_Prism_tree& __x) noexcept {
        if (__x._M_count == 0) return;
        _M_head._M_next = __x._M_head._M_next;
        _M_head._M_prev = __x._M_head._M_prev;
        _M_head._M_next->_M_prev = &_M_head;
        _M_head._M_prev->_M_next = &_M_head;
        _M_count = __x._M_count;
        for (_Base* __y = _M_head._M_next; __y != &_M_head; __y = __y->_M_next) __y->_M_owner = &_M_head;
        __x._M_reset();
    }
    void _M_swap(_Prism_tree& __x) noexcept(is_nothrow_swappable_v<_Compare>) {
        _Prism_tree __tmp{_M_cmp};
        __tmp._M_steal(*this);
        _M_steal(__x);
        __x._M_steal(__tmp);
        using std::swap;
        swap(_M_cmp, __x._M_cmp);
    }

    // -- preconditions ---------------------------------------------------------
    // __p is a position of this container (a hint, an erase or insert position)
    bool _M_owns(const _Base* __p) const noexcept { return __p->_M_owner == &_M_head; }
    void _M_check_position(const _Base* __p) const noexcept { __glibcxx_assert(this->_M_owns(__p)); }
    void _M_check_element(const _Base* __p) const noexcept {
        __glibcxx_assert(this->_M_owns(__p) && __p->_M_dereferenceable());
    }

    // -- lookup ----------------------------------------------------------------
    template <typename _Kt>
    _Base* _M_lower(const _Kt& __k) const {
        _Base* __x = _M_head._M_next;
        while (__x != &_M_head && _M_cmp(_S_key(__x), __k)) __x = __x->_M_next;
        return __x;
    }
    template <typename _Kt>
    _Base* _M_upper(const _Kt& __k) const {
        _Base* __x = _M_head._M_next;
        while (__x != &_M_head && !_M_cmp(__k, _S_key(__x))) __x = __x->_M_next;
        return __x;
    }
    template <typename _Kt>
    _Base* _M_find(const _Kt& __k) const {
        _Base* __x = _M_lower(__k);
        return _M_equiv(__x, __k) ? __x : const_cast<_Base*>(&_M_head);
    }
    template <typename _Kt>
    size_type _M_count_of(const _Kt& __k) const {
        _Base* __x = _M_lower(__k);
        size_type __n = 0;
        for (; __x != &_M_head && !_M_cmp(__k, _S_key(__x)); __x = __x->_M_next) ++__n;
        return __n;
    }
    template <typename _Kt>
    pair<_Base*, _Base*> _M_equal_range(const _Kt& __k) const {
        _Base* __lo = _M_lower(__k);
        _Base* __hi = __lo;
        while (__hi != &_M_head && !_M_cmp(__k, _S_key(__hi))) __hi = __hi->_M_next;
        return {__lo, __hi};
    }
    // libstdc++'s _M_get_insert_hint_equal_pos (+ _M_insert_equal_lower):
    // where a hinted multi insert of __k goes.
    _Base* _M_pos_hint_equal(_Base* __h, const _Key& __k) const {
        _Base* const __end = const_cast<_Base*>(&_M_head);
        if (__h == __end) {
            if (_M_count > 0 && !_M_cmp(__k, _S_key(_M_head._M_prev))) return __end;
            return _M_upper(__k);
        }
        if (!_M_cmp(_S_key(__h), __k)) {
            if (__h == _M_head._M_next) return __h;
            if (!_M_cmp(__k, _S_key(__h->_M_prev))) return __h;
            return _M_upper(__k);
        }
        if (__h == _M_head._M_prev) return __end;
        if (!_M_cmp(_S_key(__h->_M_next), __k)) return __h->_M_next;
        return _M_lower(__k);
    }

    // -- insertion ---------------------------------------------------------------
    // look up first, construct only for a new key (_M_insert_unique)
    // Unique insertion results are the node (the new one or the existing
    // equivalent one) and whether it was inserted: _M_inserted is set by
    // the last insertion (a member, not a pair: see above).
    bool _M_inserted = false;
    // look up first, construct only for a new key (_M_insert_unique)
    template <typename _Arg>
    _Base* _M_insert_unique(_Arg&& __v) {
        const _Key& __k = _KeyOfValue::_S_get(__v);
        _Base* __p = _M_lower(__k);
        _M_inserted = !_M_equiv(__p, __k);
        if (!_M_inserted) return __p;
        return _M_link(__p, _M_create(std::forward<_Arg>(__v)));
    }
    template <typename _Arg>
    _Base* _M_insert_unique_hint(const _Base* __h, _Arg&& __v) {
        _M_check_position(__h);
        return _M_insert_unique(std::forward<_Arg>(__v));
    }
    // construct first, drop the node if the key exists (_M_emplace_unique)
    template <typename... _Args>
    _Base* _M_emplace_unique(_Args&&... __args) {
        _Node* __n = _M_create(std::forward<_Args>(__args)...);
        _Base* __p = _M_pos_or_drop<_S_at_lower>(__n);
        _M_inserted = !_M_equiv_or_drop(__p, __n);
        if (!_M_inserted) {
            _M_drop(__n);
            return __p;
        }
        return _M_link(__p, __n);
    }
    template <typename... _Args>
    _Base* _M_emplace_hint_unique(const _Base* __h, _Args&&... __args) {
        _M_check_position(__h);
        return _M_emplace_unique(std::forward<_Args>(__args)...);
    }
    // construct at __p, known to be where a new key goes (callers looked it up)
    template <typename... _Args>
    _Base* _M_emplace_at(_Base* __p, _Args&&... __args) {
        return _M_link(__p, _M_create(std::forward<_Args>(__args)...));
    }
    template <typename _Arg>
    _Base* _M_insert_equal(_Arg&& __v) {
        _Base* __p = _M_upper(_KeyOfValue::_S_get(__v));
        return _M_link(__p, _M_create(std::forward<_Arg>(__v)));
    }
    template <typename _Arg>
    _Base* _M_insert_equal_hint(_Base* __h, _Arg&& __v) {
        _M_check_position(__h);
        _Base* __p = _M_pos_hint_equal(__h, _KeyOfValue::_S_get(__v));
        return _M_link(__p, _M_create(std::forward<_Arg>(__v)));
    }
    template <typename... _Args>
    _Base* _M_emplace_equal(_Args&&... __args) {
        _Node* __n = _M_create(std::forward<_Args>(__args)...);
        return _M_link(_M_pos_or_drop<_S_at_upper>(__n), __n);
    }
    template <typename... _Args>
    _Base* _M_emplace_hint_equal(_Base* __h, _Args&&... __args) {
        _M_check_position(__h);
        _Node* __n = _M_create(std::forward<_Args>(__args)...);
        return _M_link(_M_hint_pos_or_drop(__h, __n), __n);
    }
    template <typename _It>
    void _M_insert_range_unique(_It __first, _It __last) {
        for (; __first != __last; ++__first) _M_insert_unique(*__first);
    }
    template <typename _It>
    void _M_insert_range_equal(_It __first, _It __last) {
        for (; __first != __last; ++__first) _M_insert_equal_hint(&_M_head, *__first);
    }

    // -- erasure -------------------------------------------------------------------
    _Base* _M_erase(const _Base* __c) noexcept {
        _M_check_element(__c);
        _Base* __p = const_cast<_Base*>(__c);
        _Base* __next = __p->_M_next;
        _M_unlink(__p);
        _M_drop(__p);
        return __next;
    }
    _Base* _M_erase(const _Base* __first, const _Base* __last) noexcept {
        _M_check_position(__first);
        _M_check_position(__last);
        if (__first == _M_head._M_next && __last == &_M_head) {
            _M_clear();
            return &_M_head;
        }
        while (__first != __last) __first = _M_erase(__first);
        return const_cast<_Base*>(__last);
    }
    template <typename _Kt>
    size_type _M_erase_key(const _Kt& __k) {
        _Base* __p = _M_lower(__k);
        if constexpr (!_Multi) {
            if (!_M_equiv(__p, __k)) return 0;
            _M_erase(__p);
            return 1;
        } else {
            size_type __n = 0;
            while (_M_equiv(__p, __k)) {
                __p = _M_erase(__p);
                ++__n;
            }
            return __n;
        }
    }

    // -- whole-container relations ---------------------------------------------------
    template <typename _It>
    static bool _S_equal(_It __f1, _It __l1, _It __f2) {
        for (; __f1 != __l1; ++__f1, (void)++__f2)
            if (!(*__f1 == *__f2)) return false;
        return true;
    }
};

}  // namespace std

#endif  // _PRISM_MODEL_TREE_H
