// PRISM operational model: the hash table behind <unordered_map> and
// <unordered_set> (roadmap 2.6, docs/PIR.md "C++ library models"). Not a public
// header: `unordered_map` and `unordered_set` in this directory include it.
//
// libstdc++'s _Hashtable pulls _Prime_rehash_policy::_M_next_bkt and
// _Hash_bytes from libstdc++.so and rehashes recursively in ways the PIR
// encoder does not inline (`NEEDS-HARNESS`). This model inlines libstdc++ 13's
// prime bucket policy (src/c++11/hashtable_c++0x.cc) and keeps elements in
// heap nodes on singly-linked bucket chains plus a global insertion-order list
// for forward iteration. Observable behaviour matches for unique/multiple keys,
// `std::allocator` heap lifetimes (erase frees the node → MEM-UAF on a stale
// iterator), `out_of_range` from `unordered_map::at`, rehash/`reserve` bucket
// counts, and load factor. Node handles, `extract`/`merge`, and other allocators
// are not modelled (the unit falls back to libstdc++).
#ifndef _PRISM_MODEL_HASH_TABLE_H
#define _PRISM_MODEL_HASH_TABLE_H 1

#pragma GCC system_header

#include <bits/c++config.h>
#include <bits/stl_algobase.h>
#include <bits/allocator.h>
#include <bits/alloc_traits.h>
#include <bits/stl_function.h>
#include <bits/stl_pair.h>
#include <bits/stl_iterator.h>
#include <bits/functexcept.h>
#include <bits/functional_hash.h>
#include <ext/aligned_buffer.h>
#include <initializer_list>
#include <type_traits>
#include <algorithm>
#include <cmath>
#include <debug/assertions.h>

namespace std _GLIBCXX_VISIBILITY(default) {

template <typename _Val>
struct _Prism_identity {
    static const _Val& _S_get(const _Val& __x) noexcept { return __x; }
};

namespace _Prism_hash_detail {

inline const unsigned long* _S_prime_list() noexcept {
    static const unsigned long __prime_list[] = {
        2ul, 3ul, 5ul, 7ul, 11ul, 13ul, 17ul, 19ul, 23ul, 29ul, 31ul, 37ul, 41ul, 43ul, 47ul, 53ul,
        59ul, 61ul, 67ul, 71ul, 73ul, 79ul, 83ul, 89ul, 97ul, 103ul, 109ul, 113ul, 127ul, 137ul, 139ul,
        149ul, 157ul, 167ul, 179ul, 193ul, 199ul, 211ul, 227ul, 241ul, 257ul, 277ul, 293ul, 313ul,
        337ul, 359ul, 383ul, 409ul, 439ul, 467ul, 503ul, 541ul, 577ul, 619ul, 661ul, 709ul, 761ul,
        823ul, 887ul, 953ul, 1031ul, 1109ul, 1193ul, 1289ul, 1381ul, 1493ul, 1613ul, 1741ul, 1879ul,
        2029ul, 2179ul, 2357ul, 2549ul, 2753ul, 2971ul, 3209ul, 3469ul, 3739ul, 4027ul, 4349ul, 4703ul,
        5087ul, 5503ul, 5953ul, 6427ul, 6949ul, 7517ul, 8123ul, 8783ul, 9497ul, 10273ul, 11113ul,
        12011ul, 12983ul, 14033ul, 15173ul, 16411ul, 17749ul, 19183ul, 20753ul, 22447ul, 24281ul,
        26267ul, 28411ul, 30727ul, 33223ul, 35933ul, 38873ul, 42043ul, 45481ul, 49201ul, 53201ul,
        57557ul, 62233ul, 67307ul, 72817ul, 78779ul, 85229ul, 92203ul, 99733ul, 107897ul, 116731ul,
        126271ul, 136607ul, 147793ul, 159871ul, 172933ul, 187091ul, 202409ul, 218971ul, 236897ul,
        256279ul, 277261ul, 299951ul, 324503ul, 351061ul, 379787ul, 410857ul, 444487ul, 480881ul,
        520241ul, 562841ul, 608903ul, 658753ul, 712697ul, 771049ul, 834181ul, 902483ul, 976369ul,
        1056323ul, 1142821ul, 1236397ul, 1337629ul, 1447153ul, 1565659ul, 1693859ul, 1832561ul,
        1982627ul, 2144977ul, 2320627ul, 2510653ul, 2716249ul, 2938679ul, 3179303ul, 3439651ul,
        3721303ul, 4026031ul, 4355707ul, 4712381ul, 5098259ul, 5515729ul, 5967347ul, 6456007ul,
        6984629ul, 7556579ul, 8175383ul, 8844859ul, 9569143ul, 10352717ul, 11200489ul, 12117689ul,
        13109983ul, 14183539ul, 15345007ul, 16601593ul, 17961079ul, 19431899ul, 21023161ul,
        22744717ul, 24607243ul, 26622317ul, 28802401ul, 31160981ul, 33712729ul, 36473443ul,
        39460231ul, 42691603ul, 46187573ul, 49969847ul, 54061849ul, 58488943ul, 63278561ul,
        68460391ul, 74066549ul, 80131819ul, 86693767ul, 93793069ul, 101473717ul, 109783337ul,
        118773397ul, 128499677ul, 139022417ul, 150406843ul, 162723577ul, 176048909ul, 190465427ul,
        206062531ul, 222936881ul, 241193053ul, 260944219ul, 282312799ul, 305431229ul, 330442829ul,
        357502601ul, 386778277ul, 418451333ul, 452718089ul, 489790921ul, 529899637ul, 573292817ul,
        620239453ul, 671030513ul, 725980837ul, 785430967ul, 849749479ul, 919334987ul, 994618837ul,
        1076067617ul, 1164186217ul, 1259520799ul, 1362662261ul, 1474249943ul, 1594975441ul,
        1725587117ul, 1866894511ul, 2019773507ul, 2185171673ul, 2364114217ul, 2557710269ul,
        2767159799ul, 2993761039ul, 3238918481ul, 3504151727ul, 3791104843ul, 4101556399ul,
        4294967291ul,
#if __SIZEOF_LONG__ != 8
        4294967291ul
#else
        6442450933ul, 8589934583ul, 12884901857ul, 17179869143ul, 25769803693ul, 34359738337ul,
        51539607367ul, 68719476731ul, 103079215087ul, 137438953447ul, 206158430123ul, 274877906899ul,
        412316860387ul, 549755813881ul, 824633720731ul, 1099511627689ul, 1649267441579ul,
        2199023255531ul, 3298534883309ul, 4398046511093ul, 6597069766607ul, 8796093022151ul,
        13194139533241ul, 17592186044399ul, 26388279066581ul, 35184372088777ul, 52776558133177ul,
        70368744177643ul, 105553116266399ul, 140737488355213ul, 211106232532861ul, 281474976710597ul,
        562949953421231ul, 1125899906842597ul, 2251799813685119ul, 4503599627370449ul,
        9007199254740881ul, 18014398509481951ul, 36028797018963913ul, 72057594037927931ul,
        144115188075855859ul, 288230376151711717ul, 576460752303423433ul, 1152921504606846883ul,
        2305843009213693951ul, 4611686018427387847ul, 9223372036854775783ul, 18446744073709551557ul,
        18446744073709551557ul
#endif
    };
    return __prime_list;
}

struct _Prism_prime_rehash_policy {
    static constexpr size_t _S_growth_factor = 2;
    float _M_max_load_factor = 1.0f;
    mutable size_t _M_next_resize = 0;

    explicit _Prism_prime_rehash_policy(float __z = 1.0f) noexcept : _M_max_load_factor(__z) {}

    float max_load_factor() const noexcept { return _M_max_load_factor; }

    size_t _M_bkt_for_elements(size_t __n) const noexcept {
        return static_cast<size_t>(std::ceil(__n / static_cast<double>(_M_max_load_factor)));
    }

    size_t _M_next_bkt(size_t __n) const {
        static const unsigned char __fast_bkt[] = {2, 2, 2, 3, 5, 5, 7, 7, 11, 11, 11, 11, 13, 13};
        if (__n < sizeof(__fast_bkt)) {
            if (__n == 0) return 1;
            _M_next_resize = static_cast<size_t>(std::floor(__fast_bkt[__n] * static_cast<double>(_M_max_load_factor)));
            return __fast_bkt[__n];
        }
        const unsigned long* __prime_list = _S_prime_list();
#if __SIZEOF_LONG__ == 8
        constexpr size_t __n_primes = 256 + 48;
#else
        constexpr size_t __n_primes = 256;
#endif
        const unsigned long* __last_prime = __prime_list + __n_primes - 1;
        const unsigned long* __next_bkt = std::lower_bound(__prime_list + 6, __last_prime, __n);
        if (__next_bkt == __last_prime)
            _M_next_resize = size_t(-1);
        else
            _M_next_resize = static_cast<size_t>(std::floor(*__next_bkt * static_cast<double>(_M_max_load_factor)));
        return *__next_bkt;
    }

    pair<bool, size_t> _M_need_rehash(size_t __n_bkt, size_t __n_elt, size_t __n_ins) const {
        if (__n_elt + __n_ins > _M_next_resize) {
            double __min_bkts = std::max<size_t>(__n_elt + __n_ins, _M_next_resize ? 0 : 11) /
                                static_cast<double>(_M_max_load_factor);
            if (__min_bkts >= static_cast<double>(__n_bkt))
                return {true, _M_next_bkt(std::max<size_t>(static_cast<size_t>(std::floor(__min_bkts)) + 1,
                                                           __n_bkt * _S_growth_factor))};
            _M_next_resize = static_cast<size_t>(std::floor(__n_bkt * static_cast<double>(_M_max_load_factor)));
            return {false, 0};
        }
        return {false, 0};
    }

    void _M_reset() noexcept { _M_next_resize = 0; }
    size_t _M_state() const noexcept { return _M_next_resize; }
    void _M_reset_state(size_t __s) const { _M_next_resize = __s; }
};

}  // namespace _Prism_hash_detail

template <typename _Val>
struct _Prism_hash_node_layout {
    void* _M_next_bucket;
    void* _M_next_iter;
    size_t _M_hash;
    __gnu_cxx::__aligned_membuf<_Val> _M_storage;
};

template <typename _Val>
struct _Prism_hash_node {
    _Prism_hash_node* _M_next_bucket = nullptr;
    _Prism_hash_node* _M_next_iter = nullptr;
    size_t _M_hash = 0;
    const _Prism_hash_node* _M_owner = nullptr;
    __gnu_cxx::__aligned_membuf<_Val> _M_storage;
    _Val* _M_valptr() noexcept { return _M_storage._M_ptr(); }
    const _Val* _M_valptr() const noexcept { return _M_storage._M_ptr(); }
};

template <typename _Val>
struct _Prism_hash_iterator {
    using value_type = _Val;
    using reference = _Val&;
    using pointer = _Val*;
    using iterator_category = forward_iterator_tag;
    using difference_type = ptrdiff_t;
    _Prism_hash_node<_Val>* _M_node = nullptr;
    _Prism_hash_iterator() = default;
    explicit _Prism_hash_iterator(_Prism_hash_node<_Val>* __n) noexcept : _M_node(__n) {}
    reference operator*() const noexcept {
        __glibcxx_assert(_M_node && _M_node->_M_owner);
        return *_M_node->_M_valptr();
    }
    pointer operator->() const noexcept {
        __glibcxx_assert(_M_node && _M_node->_M_owner);
        return _M_node->_M_valptr();
    }
    _Prism_hash_iterator& operator++() noexcept {
        __glibcxx_assert(_M_node);
        _M_node = _M_node->_M_next_iter;
        return *this;
    }
    _Prism_hash_iterator operator++(int) noexcept {
        _Prism_hash_iterator __t = *this;
        ++*this;
        return __t;
    }
    friend bool operator==(const _Prism_hash_iterator& __a, const _Prism_hash_iterator& __b) noexcept {
        return __a._M_node == __b._M_node;
    }
};

template <typename _Val>
struct _Prism_hash_const_iterator {
    using value_type = _Val;
    using reference = const _Val&;
    using pointer = const _Val*;
    using iterator = _Prism_hash_iterator<_Val>;
    using iterator_category = forward_iterator_tag;
    using difference_type = ptrdiff_t;
    const _Prism_hash_node<_Val>* _M_node = nullptr;
    _Prism_hash_const_iterator() = default;
    explicit _Prism_hash_const_iterator(const _Prism_hash_node<_Val>* __n) noexcept : _M_node(__n) {}
    _Prism_hash_const_iterator(const iterator& __it) noexcept : _M_node(__it._M_node) {}
    reference operator*() const noexcept {
        __glibcxx_assert(_M_node && _M_node->_M_owner);
        return *_M_node->_M_valptr();
    }
    pointer operator->() const noexcept {
        __glibcxx_assert(_M_node && _M_node->_M_owner);
        return _M_node->_M_valptr();
    }
    _Prism_hash_const_iterator& operator++() noexcept {
        __glibcxx_assert(_M_node);
        _M_node = _M_node->_M_next_iter;
        return *this;
    }
    _Prism_hash_const_iterator operator++(int) noexcept {
        _Prism_hash_const_iterator __t = *this;
        ++*this;
        return __t;
    }
    friend bool operator==(const _Prism_hash_const_iterator& __a, const _Prism_hash_const_iterator& __b) noexcept {
        return __a._M_node == __b._M_node;
    }
};

template <typename _Key, typename _Val, typename _KeyOfValue, typename _Hash, typename _Equal, typename _Alloc,
          bool _Unique>
class _Prism_hashtable {
    static_assert(is_same_v<_Alloc, allocator<_Val>>,
                  "PRISM unordered model: only std::allocator is modelled");

public:
    using key_type = _Key;
    using value_type = _Val;
    using size_type = size_t;
    using hasher = _Hash;
    using key_equal = _Equal;
    using allocator_type = _Alloc;
    using iterator = _Prism_hash_iterator<_Val>;
    using const_iterator = _Prism_hash_const_iterator<_Val>;
    using difference_type = ptrdiff_t;

private:
    using _Node = _Prism_hash_node<_Val>;
    using _Node_alloc = allocator<_Node>;
    using _Node_traits = allocator_traits<_Node_alloc>;

    [[no_unique_address]] _Hash _M_hash;
    [[no_unique_address]] _Equal _M_eq;
    [[no_unique_address]] _Node_alloc _M_na;
    _Prism_hash_detail::_Prism_prime_rehash_policy _M_rehash_policy;
    size_type _M_bucket_count = 1;
    size_type _M_element_count = 0;
    _Node** _M_buckets = nullptr;
    _Node* _M_single_bucket_head = nullptr;
    _Node* _M_iter_head = nullptr;
    _Node* _M_iter_tail = nullptr;

    static const _Key& _S_key(const _Val& __v) noexcept { return _KeyOfValue::_S_get(__v); }

    size_type _M_bucket_index(size_t __code) const noexcept { return __code % _M_bucket_count; }

    size_t _M_hash_code(const _Key& __k) const { return static_cast<size_t>(_M_hash(__k)); }

    template <typename _Kt>
    size_t _M_hash_code_tr(const _Kt& __k) const {
        return static_cast<size_t>(_M_hash(__k));
    }

    void _M_reset_buckets() noexcept {
        _M_rehash_policy._M_reset();
        _M_bucket_count = 1;
        _M_single_bucket_head = nullptr;
        _M_buckets = &_M_single_bucket_head;
        _M_iter_head = _M_iter_tail = nullptr;
        _M_element_count = 0;
    }

    _Node** _M_allocate_buckets(size_type __n) {
        _Node** __p = static_cast<_Node**>(::operator new(__n * sizeof(_Node*)));
        for (size_type __i = 0; __i < __n; ++__i) __p[__i] = nullptr;
        return __p;
    }

    void _M_deallocate_buckets() noexcept {
        if (_M_bucket_count > 1) ::operator delete(_M_buckets);
    }

    template <typename... _Args>
    _Node* _M_create(_Args&&... __args) {
        _Node* __n = _Node_traits::allocate(_M_na, 1);
        ::new (static_cast<void*>(__n)) _Node;
        __n->_M_owner = reinterpret_cast<const _Prism_hash_node<_Val>*>(static_cast<const void*>(this));
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

    void _M_drop(_Node* __n) noexcept {
        std::destroy_at(__n->_M_valptr());
        _Node_traits::deallocate(_M_na, __n, 1);
    }

    void _M_unlink_iter(_Node* __n) noexcept {
        if (_M_iter_head == __n) _M_iter_head = __n->_M_next_iter;
        else {
            for (_Node* __p = _M_iter_head; __p; __p = __p->_M_next_iter)
                if (__p->_M_next_iter == __n) {
                    __p->_M_next_iter = __n->_M_next_iter;
                    break;
                }
        }
        if (_M_iter_tail == __n) _M_iter_tail = _M_iter_head ? __n->_M_next_iter : nullptr;
    }

    void _M_link_iter(_Node* __n) noexcept {
        __n->_M_next_iter = nullptr;
        if (!_M_iter_head)
            _M_iter_head = _M_iter_tail = __n;
        else {
            _M_iter_tail->_M_next_iter = __n;
            _M_iter_tail = __n;
        }
    }

    void _M_insert_bucket_begin(size_type __bkt, _Node* __n) noexcept {
        __n->_M_next_bucket = _M_buckets[__bkt];
        _M_buckets[__bkt] = __n;
    }

    _Node* _M_find_node(size_type __bkt, const _Key& __k, size_t __code) const {
        for (_Node* __p = _M_buckets[__bkt]; __p; __p = __p->_M_next_bucket)
            if (__p->_M_hash == __code && _M_eq(_S_key(*__p->_M_valptr()), __k)) return __p;
        return nullptr;
    }

    template <typename _Kt>
    _Node* _M_find_node_tr(size_type __bkt, const _Kt& __k, size_t __code) const {
        for (_Node* __p = _M_buckets[__bkt]; __p; __p = __p->_M_next_bucket)
            if (__p->_M_hash == __code && _M_eq(__k, _S_key(*__p->_M_valptr()))) return __p;
        return nullptr;
    }

    void _M_do_rehash(size_type __bkt_count) {
        const size_t __saved = _M_rehash_policy._M_state();
        __bkt_count = _M_rehash_policy._M_next_bkt(std::max(_M_rehash_policy._M_bkt_for_elements(_M_element_count + 1), __bkt_count));
        if (__bkt_count == _M_bucket_count) {
            _M_rehash_policy._M_reset_state(__saved);
            return;
        }
        _Node** __new = _M_allocate_buckets(__bkt_count);
        for (_Node* __p = _M_iter_head; __p; __p = __p->_M_next_iter) {
            size_type __bkt = __p->_M_hash % __bkt_count;
            __p->_M_next_bucket = __new[__bkt];
            __new[__bkt] = __p;
        }
        _M_deallocate_buckets();
        _M_buckets = __new;
        _M_bucket_count = __bkt_count;
    }

    void _M_prepare_insert(size_t __n_ins) {
        auto __need = _M_rehash_policy._M_need_rehash(_M_bucket_count, _M_element_count, __n_ins);
        if (__need.first) _M_do_rehash(_M_rehash_policy._M_next_bkt(__need.second));
    }

    iterator _M_insert_node(_Node* __n, size_t __code) {
        _M_prepare_insert(1);
        __code = __n->_M_hash = __code;
        size_type __bkt = _M_bucket_index(__code);
        if constexpr (_Unique) {
            if (_Node* __ex = _M_find_node(__bkt, _S_key(*__n->_M_valptr()), __code)) {
                _M_drop(__n);
                return iterator(__ex);
            }
        }
        _M_insert_bucket_begin(__bkt, __n);
        _M_link_iter(__n);
        ++_M_element_count;
        return iterator(__n);
    }

public:
    _Prism_hashtable() { _M_reset_buckets(); }
    _Prism_hashtable(size_type __n, const _Hash& __hf, const _Equal& __eq, const _Alloc& = _Alloc())
        : _M_hash(__hf), _M_eq(__eq) {
        _M_reset_buckets();
        if (__n > 0) {
            size_type __b = _M_rehash_policy._M_next_bkt(__n);
            if (__b > 1) {
                _M_deallocate_buckets();
                _M_buckets = _M_allocate_buckets(__b);
                _M_bucket_count = __b;
            }
        }
    }

    _Prism_hashtable(const _Prism_hashtable& __x)
        : _M_hash(__x._M_hash), _M_eq(__x._M_eq), _M_rehash_policy(__x._M_rehash_policy) {
        _M_reset_buckets();
        for (const _Node* __s = __x._M_iter_head; __s; __s = __s->_M_next_iter) (void)emplace(*__s->_M_valptr());
    }

    _Prism_hashtable(_Prism_hashtable&& __x) noexcept
        : _M_hash(std::move(__x._M_hash)), _M_eq(std::move(__x._M_eq)), _M_rehash_policy(__x._M_rehash_policy),
          _M_bucket_count(__x._M_bucket_count), _M_element_count(__x._M_element_count), _M_buckets(__x._M_buckets),
          _M_iter_head(__x._M_iter_head), _M_iter_tail(__x._M_iter_tail) {
        if (__x._M_bucket_count == 1) {
            _M_buckets = &_M_single_bucket_head;
            _M_single_bucket_head = __x._M_single_bucket_head;
        }
        __x._M_reset_buckets();
    }

    ~_Prism_hashtable() {
        clear();
        _M_deallocate_buckets();
    }

    _Prism_hashtable& operator=(const _Prism_hashtable& __x) {
        if (this != &__x) {
            _Prism_hashtable(__x).swap(*this);
        }
        return *this;
    }

    _Prism_hashtable& operator=(_Prism_hashtable&& __x) noexcept {
        clear();
        _M_deallocate_buckets();
        _M_hash = std::move(__x._M_hash);
        _M_eq = std::move(__x._M_eq);
        _M_rehash_policy = __x._M_rehash_policy;
        _M_bucket_count = __x._M_bucket_count;
        _M_element_count = __x._M_element_count;
        _M_buckets = __x._M_buckets;
        _M_iter_head = __x._M_iter_head;
        _M_iter_tail = __x._M_iter_tail;
        if (__x._M_bucket_count == 1) {
            _M_buckets = &_M_single_bucket_head;
            _M_single_bucket_head = __x._M_single_bucket_head;
        }
        __x._M_reset_buckets();
        return *this;
    }

    hasher hash_function() const { return _M_hash; }
    key_equal key_eq() const { return _M_eq; }
    allocator_type get_allocator() const noexcept { return allocator_type(); }

    iterator begin() noexcept { return iterator(_M_iter_head); }
    const_iterator begin() const noexcept { return const_iterator(_M_iter_head); }
    const_iterator cbegin() const noexcept { return begin(); }
    iterator end() noexcept { return iterator(nullptr); }
    const_iterator end() const noexcept { return const_iterator(nullptr); }
    const_iterator cend() const noexcept { return end(); }

    bool empty() const noexcept { return _M_element_count == 0; }
    size_type size() const noexcept { return _M_element_count; }
    size_type max_size() const noexcept { return size_type(-1) / sizeof(_Prism_hash_node_layout<_Val>); }
    size_type bucket_count() const noexcept { return _M_bucket_count; }
    float load_factor() const noexcept {
        return _M_bucket_count ? static_cast<float>(_M_element_count) / static_cast<float>(_M_bucket_count) : 0.f;
    }
    float max_load_factor() const noexcept { return _M_rehash_policy.max_load_factor(); }
    void max_load_factor(float __z) {
        if (__z <= 0.f) __throw_out_of_range(__N("invalid hash load factor"));
        _M_rehash_policy._M_max_load_factor = __z;
    }

    void clear() noexcept {
        while (_M_iter_head) {
            _Node* __n = _M_iter_head;
            _M_iter_head = __n->_M_next_iter;
            _M_drop(__n);
        }
        _M_iter_tail = nullptr;
        _M_element_count = 0;
        for (size_type __i = 0; __i < _M_bucket_count; ++__i) _M_buckets[__i] = nullptr;
    }

    void reserve(size_type __n) { rehash(_M_rehash_policy._M_bkt_for_elements(__n)); }

    void rehash(size_type __n) {
        if (__n > _M_bucket_count) _M_do_rehash(__n);
    }

    void swap(_Prism_hashtable& __x) noexcept {
        using std::swap;
        swap(_M_hash, __x._M_hash);
        swap(_M_eq, __x._M_eq);
        swap(_M_rehash_policy, __x._M_rehash_policy);
        swap(_M_bucket_count, __x._M_bucket_count);
        swap(_M_element_count, __x._M_element_count);
        swap(_M_buckets, __x._M_buckets);
        swap(_M_iter_head, __x._M_iter_head);
        swap(_M_iter_tail, __x._M_iter_tail);
    }

    template <typename... _Args>
    pair<iterator, bool> emplace(_Args&&... __args) {
        _Node* __n = _M_create(std::forward<_Args>(__args)...);
        const _Key& __k = _S_key(*__n->_M_valptr());
        size_t __code = _M_hash_code(__k);
        _M_prepare_insert(1);
        size_type __bkt = _M_bucket_index(__code);
        if constexpr (_Unique) {
            if (_Node* __ex = _M_find_node(__bkt, __k, __code)) {
                _M_drop(__n);
                return {iterator(__ex), false};
            }
        }
        __n->_M_hash = __code;
        _M_insert_bucket_begin(__bkt, __n);
        _M_link_iter(__n);
        ++_M_element_count;
        return {iterator(__n), true};
    }

    template <typename... _Args>
    iterator emplace_hint(const_iterator, _Args&&... __args) {
        return emplace(std::forward<_Args>(__args)...).first;
    }

    pair<iterator, bool> insert(const value_type& __x) {
        if constexpr (_Unique) return emplace(__x);
        else {
            emplace(__x);
            return {iterator(_M_iter_tail), true};
        }
    }

    pair<iterator, bool> insert(value_type&& __x) {
        if constexpr (_Unique) return emplace(std::move(__x));
        else {
            emplace(std::move(__x));
            return {iterator(_M_iter_tail), true};
        }
    }

    template <typename _InputIterator>
    void insert(_InputIterator __f, _InputIterator __l) {
        for (; __f != __l; ++__f) (void)insert(*__f);
    }

    iterator erase(const_iterator __it) {
        __glibcxx_assert(__it._M_node && __it._M_node->_M_owner);
        _Node* __n = const_cast<_Node*>(__it._M_node);
        iterator __res(__n->_M_next_iter);
        size_type __bkt = _M_bucket_index(__n->_M_hash);
        _Node** __prev = &_M_buckets[__bkt];
        while (*__prev && *__prev != __n) __prev = &(*__prev)->_M_next_bucket;
        if (*__prev) *__prev = __n->_M_next_bucket;
        _M_unlink_iter(__n);
        _M_drop(__n);
        --_M_element_count;
        return __res;
    }

    size_type erase(const key_type& __k) {
        size_t __code = _M_hash_code(__k);
        size_type __bkt = _M_bucket_index(__code);
        size_type __n = 0;
        _Node** __prev = &_M_buckets[__bkt];
        while (*__prev) {
            _Node* __cur = *__prev;
            if (__cur->_M_hash == __code && _M_eq(_S_key(*__cur->_M_valptr()), __k)) {
                *__prev = __cur->_M_next_bucket;
                _M_unlink_iter(__cur);
                _M_drop(__cur);
                ++__n;
                --_M_element_count;
                if constexpr (_Unique) break;
            } else
                __prev = &(*__prev)->_M_next_bucket;
        }
        return __n;
    }

    iterator find(const key_type& __k) {
        size_t __code = _M_hash_code(__k);
        _Node* __p = _M_find_node(_M_bucket_index(__code), __k, __code);
        return iterator(__p);
    }

    const_iterator find(const key_type& __k) const {
        size_t __code = _M_hash_code(__k);
        _Node* __p = _M_find_node(_M_bucket_index(__code), __k, __code);
        return const_iterator(__p);
    }

    template <typename _Kt>
    iterator find(const _Kt& __k) {
        size_t __code = _M_hash_code_tr(__k);
        _Node* __p = _M_find_node_tr(_M_bucket_index(__code), __k, __code);
        return iterator(__p);
    }

    template <typename _Kt>
    const_iterator find(const _Kt& __k) const {
        size_t __code = _M_hash_code_tr(__k);
        _Node* __p = _M_find_node_tr(_M_bucket_index(__code), __k, __code);
        return const_iterator(__p);
    }

    size_type count(const key_type& __k) const {
        size_t __code = _M_hash_code(__k);
        size_type __c = 0;
        for (_Node* __p = _M_buckets[_M_bucket_index(__code)]; __p; __p = __p->_M_next_bucket)
            if (__p->_M_hash == __code && _M_eq(_S_key(*__p->_M_valptr()), __k)) ++__c;
        return __c;
    }

    bool contains(const key_type& __k) const { return find(__k) != end(); }

    pair<iterator, iterator> equal_range(const key_type& __k) {
        iterator __f = find(__k);
        if (__f == end()) return {__f, __f};
        iterator __l = __f;
        size_t __code = _M_hash_code(__k);
        while (__l._M_node && __l._M_node->_M_next_iter && __l._M_node->_M_next_iter->_M_hash == __code &&
               _M_eq(_S_key(*__l._M_node->_M_next_iter->_M_valptr()), __k))
            ++__l;
        if (__l._M_node) ++__l;
        return {__f, __l};
    }

    pair<const_iterator, const_iterator> equal_range(const key_type& __k) const {
        const_iterator __f = find(__k);
        if (__f == end()) return {__f, __f};
        const_iterator __l = __f;
        size_t __code = _M_hash_code(__k);
        while (__l._M_node && __l._M_node->_M_next_iter && __l._M_node->_M_next_iter->_M_hash == __code &&
               _M_eq(_S_key(*__l._M_node->_M_next_iter->_M_valptr()), __k))
            ++__l;
        if (__l._M_node) ++__l;
        return {__f, __l};
    }
};

}  // namespace std

#endif  // _PRISM_MODEL_HASH_TABLE_H
