// PRISM operational model: the hash table behind <unordered_map> and
// <unordered_set> (roadmap 2.6, docs/PIR.md "C++ library models"). Not a public
// header: `unordered_map` and `unordered_set` in this directory include it.
//
// libstdc++'s _Hashtable pulls _Prime_rehash_policy::_M_next_bkt and
// _Hash_bytes from libstdc++.so and rehashes recursively in ways the PIR
// encoder does not inline (`NEEDS-HARNESS`). This model inlines libstdc++ 13's
// prime bucket policy (src/c++11/hashtable_c++0x.cc) and keeps elements in
// heap nodes on singly-linked bucket chains plus a global insertion-order list
// for forward iteration. Observable behaviour matches libstdc++ 13 for
// unique/multiple keys (equivalent multi keys sit together in iteration order,
// so equal_range is one span), `std::allocator` heap lifetimes (erase frees
// that node → MEM-UAF on a stale iterator; iterators to other elements stay
// valid), `out_of_range` from `unordered_map::at`, rehash/`reserve` bucket
// counts, and load factor. Rehash keeps the same nodes, so iterators are not
// invalidated here — a harness must not claim rehash frees or drops them.
// Node handles, `extract`/`merge`, and allocators other than `std::allocator`
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

    // Integer fast path for the default max_load_factor 1.0: below 2^53 the
    // double round trips (n / 1.0, ceil, floor(n * 1.0)) are exact, so these
    // equal libstdc++'s results bit for bit. It keeps floating point (costly
    // bit-blasting, unordered_map_contracts timeouts) out of the encoding.
    static constexpr size_t _S_exact = size_t(1) << 53;
    bool _M_unit() const noexcept { return _M_max_load_factor == 1.0f; }
    size_t _M_floor_scaled(size_t __b) const noexcept {
        if (_M_unit() && __b < _S_exact) return __b;
        return static_cast<size_t>(std::floor(__b * static_cast<double>(_M_max_load_factor)));
    }

    size_t _M_bkt_for_elements(size_t __n) const noexcept {
        if (_M_unit() && __n < _S_exact) return __n;
        return static_cast<size_t>(std::ceil(__n / static_cast<double>(_M_max_load_factor)));
    }

    size_t _M_next_bkt(size_t __n) const {
        static const unsigned char __fast_bkt[] = {2, 2, 2, 3, 5, 5, 7, 7, 11, 11, 11, 11, 13, 13};
        if (__n < sizeof(__fast_bkt)) {
            if (__n == 0) return 1;
            _M_next_resize = _M_floor_scaled(__fast_bkt[__n]);
            return __fast_bkt[__n];
        }
#if __SIZEOF_LONG__ == 8
        // BEGIN generated by tools/models/gen_prime_lb
        // std::lower_bound(__prime_list + 6, __last_prime, __n) without a symbolic
        // table index: branch on groups of 16 primes, masks keep the smallest
        // prime >= __n within the group. Equal to lower_bound for every __n.
        unsigned long __p, __m;
        if (__n <= 79ul) {
            __p = 79ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 73ul); __p = (__p & ~__m) | (73ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 71ul); __p = (__p & ~__m) | (71ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 67ul); __p = (__p & ~__m) | (67ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 61ul); __p = (__p & ~__m) | (61ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 59ul); __p = (__p & ~__m) | (59ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 53ul); __p = (__p & ~__m) | (53ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 47ul); __p = (__p & ~__m) | (47ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 43ul); __p = (__p & ~__m) | (43ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 41ul); __p = (__p & ~__m) | (41ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 37ul); __p = (__p & ~__m) | (37ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 31ul); __p = (__p & ~__m) | (31ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 29ul); __p = (__p & ~__m) | (29ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 23ul); __p = (__p & ~__m) | (23ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 19ul); __p = (__p & ~__m) | (19ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 17ul); __p = (__p & ~__m) | (17ul & __m);
        }
        else if (__n <= 211ul) {
            __p = 211ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 199ul); __p = (__p & ~__m) | (199ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 193ul); __p = (__p & ~__m) | (193ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 179ul); __p = (__p & ~__m) | (179ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 167ul); __p = (__p & ~__m) | (167ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 157ul); __p = (__p & ~__m) | (157ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 149ul); __p = (__p & ~__m) | (149ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 139ul); __p = (__p & ~__m) | (139ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 137ul); __p = (__p & ~__m) | (137ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 127ul); __p = (__p & ~__m) | (127ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 113ul); __p = (__p & ~__m) | (113ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 109ul); __p = (__p & ~__m) | (109ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 103ul); __p = (__p & ~__m) | (103ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 97ul); __p = (__p & ~__m) | (97ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 89ul); __p = (__p & ~__m) | (89ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 83ul); __p = (__p & ~__m) | (83ul & __m);
        }
        else if (__n <= 619ul) {
            __p = 619ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 577ul); __p = (__p & ~__m) | (577ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 541ul); __p = (__p & ~__m) | (541ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 503ul); __p = (__p & ~__m) | (503ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 467ul); __p = (__p & ~__m) | (467ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 439ul); __p = (__p & ~__m) | (439ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 409ul); __p = (__p & ~__m) | (409ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 383ul); __p = (__p & ~__m) | (383ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 359ul); __p = (__p & ~__m) | (359ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 337ul); __p = (__p & ~__m) | (337ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 313ul); __p = (__p & ~__m) | (313ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 293ul); __p = (__p & ~__m) | (293ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 277ul); __p = (__p & ~__m) | (277ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 257ul); __p = (__p & ~__m) | (257ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 241ul); __p = (__p & ~__m) | (241ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 227ul); __p = (__p & ~__m) | (227ul & __m);
        }
        else if (__n <= 2029ul) {
            __p = 2029ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 1879ul); __p = (__p & ~__m) | (1879ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1741ul); __p = (__p & ~__m) | (1741ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1613ul); __p = (__p & ~__m) | (1613ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1493ul); __p = (__p & ~__m) | (1493ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1381ul); __p = (__p & ~__m) | (1381ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1289ul); __p = (__p & ~__m) | (1289ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1193ul); __p = (__p & ~__m) | (1193ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1109ul); __p = (__p & ~__m) | (1109ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1031ul); __p = (__p & ~__m) | (1031ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 953ul); __p = (__p & ~__m) | (953ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 887ul); __p = (__p & ~__m) | (887ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 823ul); __p = (__p & ~__m) | (823ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 761ul); __p = (__p & ~__m) | (761ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 709ul); __p = (__p & ~__m) | (709ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 661ul); __p = (__p & ~__m) | (661ul & __m);
        }
        else if (__n <= 6949ul) {
            __p = 6949ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 6427ul); __p = (__p & ~__m) | (6427ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 5953ul); __p = (__p & ~__m) | (5953ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 5503ul); __p = (__p & ~__m) | (5503ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 5087ul); __p = (__p & ~__m) | (5087ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 4703ul); __p = (__p & ~__m) | (4703ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 4349ul); __p = (__p & ~__m) | (4349ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 4027ul); __p = (__p & ~__m) | (4027ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 3739ul); __p = (__p & ~__m) | (3739ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 3469ul); __p = (__p & ~__m) | (3469ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 3209ul); __p = (__p & ~__m) | (3209ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2971ul); __p = (__p & ~__m) | (2971ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2753ul); __p = (__p & ~__m) | (2753ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2549ul); __p = (__p & ~__m) | (2549ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2357ul); __p = (__p & ~__m) | (2357ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2179ul); __p = (__p & ~__m) | (2179ul & __m);
        }
        else if (__n <= 24281ul) {
            __p = 24281ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 22447ul); __p = (__p & ~__m) | (22447ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 20753ul); __p = (__p & ~__m) | (20753ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 19183ul); __p = (__p & ~__m) | (19183ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 17749ul); __p = (__p & ~__m) | (17749ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 16411ul); __p = (__p & ~__m) | (16411ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 15173ul); __p = (__p & ~__m) | (15173ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 14033ul); __p = (__p & ~__m) | (14033ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 12983ul); __p = (__p & ~__m) | (12983ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 12011ul); __p = (__p & ~__m) | (12011ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 11113ul); __p = (__p & ~__m) | (11113ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 10273ul); __p = (__p & ~__m) | (10273ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 9497ul); __p = (__p & ~__m) | (9497ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 8783ul); __p = (__p & ~__m) | (8783ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 8123ul); __p = (__p & ~__m) | (8123ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 7517ul); __p = (__p & ~__m) | (7517ul & __m);
        }
        else if (__n <= 85229ul) {
            __p = 85229ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 78779ul); __p = (__p & ~__m) | (78779ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 72817ul); __p = (__p & ~__m) | (72817ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 67307ul); __p = (__p & ~__m) | (67307ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 62233ul); __p = (__p & ~__m) | (62233ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 57557ul); __p = (__p & ~__m) | (57557ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 53201ul); __p = (__p & ~__m) | (53201ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 49201ul); __p = (__p & ~__m) | (49201ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 45481ul); __p = (__p & ~__m) | (45481ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 42043ul); __p = (__p & ~__m) | (42043ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 38873ul); __p = (__p & ~__m) | (38873ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 35933ul); __p = (__p & ~__m) | (35933ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 33223ul); __p = (__p & ~__m) | (33223ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 30727ul); __p = (__p & ~__m) | (30727ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 28411ul); __p = (__p & ~__m) | (28411ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 26267ul); __p = (__p & ~__m) | (26267ul & __m);
        }
        else if (__n <= 299951ul) {
            __p = 299951ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 277261ul); __p = (__p & ~__m) | (277261ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 256279ul); __p = (__p & ~__m) | (256279ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 236897ul); __p = (__p & ~__m) | (236897ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 218971ul); __p = (__p & ~__m) | (218971ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 202409ul); __p = (__p & ~__m) | (202409ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 187091ul); __p = (__p & ~__m) | (187091ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 172933ul); __p = (__p & ~__m) | (172933ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 159871ul); __p = (__p & ~__m) | (159871ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 147793ul); __p = (__p & ~__m) | (147793ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 136607ul); __p = (__p & ~__m) | (136607ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 126271ul); __p = (__p & ~__m) | (126271ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 116731ul); __p = (__p & ~__m) | (116731ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 107897ul); __p = (__p & ~__m) | (107897ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 99733ul); __p = (__p & ~__m) | (99733ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 92203ul); __p = (__p & ~__m) | (92203ul & __m);
        }
        else if (__n <= 1056323ul) {
            __p = 1056323ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 976369ul); __p = (__p & ~__m) | (976369ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 902483ul); __p = (__p & ~__m) | (902483ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 834181ul); __p = (__p & ~__m) | (834181ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 771049ul); __p = (__p & ~__m) | (771049ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 712697ul); __p = (__p & ~__m) | (712697ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 658753ul); __p = (__p & ~__m) | (658753ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 608903ul); __p = (__p & ~__m) | (608903ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 562841ul); __p = (__p & ~__m) | (562841ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 520241ul); __p = (__p & ~__m) | (520241ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 480881ul); __p = (__p & ~__m) | (480881ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 444487ul); __p = (__p & ~__m) | (444487ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 410857ul); __p = (__p & ~__m) | (410857ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 379787ul); __p = (__p & ~__m) | (379787ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 351061ul); __p = (__p & ~__m) | (351061ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 324503ul); __p = (__p & ~__m) | (324503ul & __m);
        }
        else if (__n <= 3721303ul) {
            __p = 3721303ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 3439651ul); __p = (__p & ~__m) | (3439651ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 3179303ul); __p = (__p & ~__m) | (3179303ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2938679ul); __p = (__p & ~__m) | (2938679ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2716249ul); __p = (__p & ~__m) | (2716249ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2510653ul); __p = (__p & ~__m) | (2510653ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2320627ul); __p = (__p & ~__m) | (2320627ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2144977ul); __p = (__p & ~__m) | (2144977ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1982627ul); __p = (__p & ~__m) | (1982627ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1832561ul); __p = (__p & ~__m) | (1832561ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1693859ul); __p = (__p & ~__m) | (1693859ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1565659ul); __p = (__p & ~__m) | (1565659ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1447153ul); __p = (__p & ~__m) | (1447153ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1337629ul); __p = (__p & ~__m) | (1337629ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1236397ul); __p = (__p & ~__m) | (1236397ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1142821ul); __p = (__p & ~__m) | (1142821ul & __m);
        }
        else if (__n <= 13109983ul) {
            __p = 13109983ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 12117689ul); __p = (__p & ~__m) | (12117689ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 11200489ul); __p = (__p & ~__m) | (11200489ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 10352717ul); __p = (__p & ~__m) | (10352717ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 9569143ul); __p = (__p & ~__m) | (9569143ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 8844859ul); __p = (__p & ~__m) | (8844859ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 8175383ul); __p = (__p & ~__m) | (8175383ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 7556579ul); __p = (__p & ~__m) | (7556579ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 6984629ul); __p = (__p & ~__m) | (6984629ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 6456007ul); __p = (__p & ~__m) | (6456007ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 5967347ul); __p = (__p & ~__m) | (5967347ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 5515729ul); __p = (__p & ~__m) | (5515729ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 5098259ul); __p = (__p & ~__m) | (5098259ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 4712381ul); __p = (__p & ~__m) | (4712381ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 4355707ul); __p = (__p & ~__m) | (4355707ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 4026031ul); __p = (__p & ~__m) | (4026031ul & __m);
        }
        else if (__n <= 46187573ul) {
            __p = 46187573ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 42691603ul); __p = (__p & ~__m) | (42691603ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 39460231ul); __p = (__p & ~__m) | (39460231ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 36473443ul); __p = (__p & ~__m) | (36473443ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 33712729ul); __p = (__p & ~__m) | (33712729ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 31160981ul); __p = (__p & ~__m) | (31160981ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 28802401ul); __p = (__p & ~__m) | (28802401ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 26622317ul); __p = (__p & ~__m) | (26622317ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 24607243ul); __p = (__p & ~__m) | (24607243ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 22744717ul); __p = (__p & ~__m) | (22744717ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 21023161ul); __p = (__p & ~__m) | (21023161ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 19431899ul); __p = (__p & ~__m) | (19431899ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 17961079ul); __p = (__p & ~__m) | (17961079ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 16601593ul); __p = (__p & ~__m) | (16601593ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 15345007ul); __p = (__p & ~__m) | (15345007ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 14183539ul); __p = (__p & ~__m) | (14183539ul & __m);
        }
        else if (__n <= 162723577ul) {
            __p = 162723577ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 150406843ul); __p = (__p & ~__m) | (150406843ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 139022417ul); __p = (__p & ~__m) | (139022417ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 128499677ul); __p = (__p & ~__m) | (128499677ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 118773397ul); __p = (__p & ~__m) | (118773397ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 109783337ul); __p = (__p & ~__m) | (109783337ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 101473717ul); __p = (__p & ~__m) | (101473717ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 93793069ul); __p = (__p & ~__m) | (93793069ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 86693767ul); __p = (__p & ~__m) | (86693767ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 80131819ul); __p = (__p & ~__m) | (80131819ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 74066549ul); __p = (__p & ~__m) | (74066549ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 68460391ul); __p = (__p & ~__m) | (68460391ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 63278561ul); __p = (__p & ~__m) | (63278561ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 58488943ul); __p = (__p & ~__m) | (58488943ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 54061849ul); __p = (__p & ~__m) | (54061849ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 49969847ul); __p = (__p & ~__m) | (49969847ul & __m);
        }
        else if (__n <= 573292817ul) {
            __p = 573292817ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 529899637ul); __p = (__p & ~__m) | (529899637ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 489790921ul); __p = (__p & ~__m) | (489790921ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 452718089ul); __p = (__p & ~__m) | (452718089ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 418451333ul); __p = (__p & ~__m) | (418451333ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 386778277ul); __p = (__p & ~__m) | (386778277ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 357502601ul); __p = (__p & ~__m) | (357502601ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 330442829ul); __p = (__p & ~__m) | (330442829ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 305431229ul); __p = (__p & ~__m) | (305431229ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 282312799ul); __p = (__p & ~__m) | (282312799ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 260944219ul); __p = (__p & ~__m) | (260944219ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 241193053ul); __p = (__p & ~__m) | (241193053ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 222936881ul); __p = (__p & ~__m) | (222936881ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 206062531ul); __p = (__p & ~__m) | (206062531ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 190465427ul); __p = (__p & ~__m) | (190465427ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 176048909ul); __p = (__p & ~__m) | (176048909ul & __m);
        }
        else if (__n <= 2019773507ul) {
            __p = 2019773507ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 1866894511ul); __p = (__p & ~__m) | (1866894511ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1725587117ul); __p = (__p & ~__m) | (1725587117ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1594975441ul); __p = (__p & ~__m) | (1594975441ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1474249943ul); __p = (__p & ~__m) | (1474249943ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1362662261ul); __p = (__p & ~__m) | (1362662261ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1259520799ul); __p = (__p & ~__m) | (1259520799ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1164186217ul); __p = (__p & ~__m) | (1164186217ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1076067617ul); __p = (__p & ~__m) | (1076067617ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 994618837ul); __p = (__p & ~__m) | (994618837ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 919334987ul); __p = (__p & ~__m) | (919334987ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 849749479ul); __p = (__p & ~__m) | (849749479ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 785430967ul); __p = (__p & ~__m) | (785430967ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 725980837ul); __p = (__p & ~__m) | (725980837ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 671030513ul); __p = (__p & ~__m) | (671030513ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 620239453ul); __p = (__p & ~__m) | (620239453ul & __m);
        }
        else if (__n <= 34359738337ul) {
            __p = 34359738337ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 25769803693ul); __p = (__p & ~__m) | (25769803693ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 17179869143ul); __p = (__p & ~__m) | (17179869143ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 12884901857ul); __p = (__p & ~__m) | (12884901857ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 8589934583ul); __p = (__p & ~__m) | (8589934583ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 6442450933ul); __p = (__p & ~__m) | (6442450933ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 4294967291ul); __p = (__p & ~__m) | (4294967291ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 4101556399ul); __p = (__p & ~__m) | (4101556399ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 3791104843ul); __p = (__p & ~__m) | (3791104843ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 3504151727ul); __p = (__p & ~__m) | (3504151727ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 3238918481ul); __p = (__p & ~__m) | (3238918481ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2993761039ul); __p = (__p & ~__m) | (2993761039ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2767159799ul); __p = (__p & ~__m) | (2767159799ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2557710269ul); __p = (__p & ~__m) | (2557710269ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2364114217ul); __p = (__p & ~__m) | (2364114217ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2185171673ul); __p = (__p & ~__m) | (2185171673ul & __m);
        }
        else if (__n <= 8796093022151ul) {
            __p = 8796093022151ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 6597069766607ul); __p = (__p & ~__m) | (6597069766607ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 4398046511093ul); __p = (__p & ~__m) | (4398046511093ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 3298534883309ul); __p = (__p & ~__m) | (3298534883309ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2199023255531ul); __p = (__p & ~__m) | (2199023255531ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1649267441579ul); __p = (__p & ~__m) | (1649267441579ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1099511627689ul); __p = (__p & ~__m) | (1099511627689ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 824633720731ul); __p = (__p & ~__m) | (824633720731ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 549755813881ul); __p = (__p & ~__m) | (549755813881ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 412316860387ul); __p = (__p & ~__m) | (412316860387ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 274877906899ul); __p = (__p & ~__m) | (274877906899ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 206158430123ul); __p = (__p & ~__m) | (206158430123ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 137438953447ul); __p = (__p & ~__m) | (137438953447ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 103079215087ul); __p = (__p & ~__m) | (103079215087ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 68719476731ul); __p = (__p & ~__m) | (68719476731ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 51539607367ul); __p = (__p & ~__m) | (51539607367ul & __m);
        }
        else if (__n <= 18014398509481951ul) {
            __p = 18014398509481951ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 9007199254740881ul); __p = (__p & ~__m) | (9007199254740881ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 4503599627370449ul); __p = (__p & ~__m) | (4503599627370449ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2251799813685119ul); __p = (__p & ~__m) | (2251799813685119ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1125899906842597ul); __p = (__p & ~__m) | (1125899906842597ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 562949953421231ul); __p = (__p & ~__m) | (562949953421231ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 281474976710597ul); __p = (__p & ~__m) | (281474976710597ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 211106232532861ul); __p = (__p & ~__m) | (211106232532861ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 140737488355213ul); __p = (__p & ~__m) | (140737488355213ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 105553116266399ul); __p = (__p & ~__m) | (105553116266399ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 70368744177643ul); __p = (__p & ~__m) | (70368744177643ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 52776558133177ul); __p = (__p & ~__m) | (52776558133177ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 35184372088777ul); __p = (__p & ~__m) | (35184372088777ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 26388279066581ul); __p = (__p & ~__m) | (26388279066581ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 17592186044399ul); __p = (__p & ~__m) | (17592186044399ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 13194139533241ul); __p = (__p & ~__m) | (13194139533241ul & __m);
        }
        else if (__n <= 9223372036854775783ul) {
            __p = 9223372036854775783ul;
            __m = 0ul - static_cast<unsigned long>(__n <= 4611686018427387847ul); __p = (__p & ~__m) | (4611686018427387847ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 2305843009213693951ul); __p = (__p & ~__m) | (2305843009213693951ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 1152921504606846883ul); __p = (__p & ~__m) | (1152921504606846883ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 576460752303423433ul); __p = (__p & ~__m) | (576460752303423433ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 288230376151711717ul); __p = (__p & ~__m) | (288230376151711717ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 144115188075855859ul); __p = (__p & ~__m) | (144115188075855859ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 72057594037927931ul); __p = (__p & ~__m) | (72057594037927931ul & __m);
            __m = 0ul - static_cast<unsigned long>(__n <= 36028797018963913ul); __p = (__p & ~__m) | (36028797018963913ul & __m);
        }
        else { _M_next_resize = size_t(-1); return 18446744073709551557ul; }
        _M_next_resize = _M_floor_scaled(__p);
        return __p;
        // END generated
#else
        const unsigned long* __prime_list = _S_prime_list();
        constexpr size_t __n_primes = 256;
        const unsigned long* __last_prime = __prime_list + __n_primes - 1;
        const unsigned long* __next_bkt = std::lower_bound(__prime_list + 6, __last_prime, __n);
        if (__next_bkt == __last_prime)
            _M_next_resize = size_t(-1);
        else
            _M_next_resize = _M_floor_scaled(*__next_bkt);
        return *__next_bkt;
#endif
    }

    pair<bool, size_t> _M_need_rehash(size_t __n_bkt, size_t __n_elt, size_t __n_ins) const {
        if (__n_elt + __n_ins > _M_next_resize) {
            if (_M_unit() && __n_elt + __n_ins < _S_exact && __n_bkt < _S_exact) {
                // __min_bkts is the integer m exactly; floor(m) + 1 = m + 1
                const size_t __m = std::max<size_t>(__n_elt + __n_ins, _M_next_resize ? 0 : 11);
                if (__m >= __n_bkt) return {true, _M_next_bkt(std::max<size_t>(__m + 1, __n_bkt * _S_growth_factor))};
                _M_next_resize = __n_bkt;
                return {false, 0};
            }
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
        // one memset, not a loop: the first insert allocates 13 buckets, more
        // iterations than the PIR unwind (4, then 8), so the loop form never
        // proved its unwinding assertion and blew up trying (unordered_map_contracts)
        __builtin_memset(static_cast<void*>(__p), 0, __n * sizeof(_Node*));
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
        if (_M_iter_head == __n) {
            _M_iter_head = __n->_M_next_iter;
            if (_M_iter_tail == __n) _M_iter_tail = nullptr;
            return;
        }
        for (_Node* __p = _M_iter_head; __p; __p = __p->_M_next_iter) {
            if (__p->_M_next_iter == __n) {
                __p->_M_next_iter = __n->_M_next_iter;
                if (_M_iter_tail == __n) _M_iter_tail = __p;
                return;
            }
        }
    }

    // Multi-keys stay contiguous: the new node follows the newest equivalent.
    void _M_link_iter_after(_Node* __pos, _Node* __n) noexcept {
        __n->_M_next_iter = __pos->_M_next_iter;
        __pos->_M_next_iter = __n;
        if (_M_iter_tail == __pos) _M_iter_tail = __n;
    }

    _Node* _M_find_eq_newest(size_type __bkt, const _Key& __k, size_t __code) const {
        for (_Node* __p = _M_buckets[__bkt]; __p; __p = __p->_M_next_bucket)
            if (__p->_M_hash == __code && _M_eq(_S_key(*__p->_M_valptr()), __k)) return __p;
        return nullptr;
    }

    void _M_adopt() noexcept {
        const auto* __self = reinterpret_cast<const _Prism_hash_node<_Val>*>(static_cast<const void*>(this));
        for (_Node* __p = _M_iter_head; __p; __p = __p->_M_next_iter) __p->_M_owner = __self;
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
        // Lighter copy (session 4): libstdc++'s _Hashtable copy keeps the
        // source bucket count, so allocate it once (no 13/29/59/... rehash
        // chain) and, for unique keys, link each node without the duplicate
        // lookup (the source already has unique keys) and reuse the cached
        // hash code. Iteration order is the source's insertion order.
        if (__x._M_bucket_count > 1) {
            _M_buckets = _M_allocate_buckets(__x._M_bucket_count);
            _M_bucket_count = __x._M_bucket_count;
        }
        try {
            for (const _Node* __s = __x._M_iter_head; __s; __s = __s->_M_next_iter) {
                if constexpr (_Unique) {
                    _Node* __n = _M_create(*__s->_M_valptr());
                    __n->_M_hash = __s->_M_hash;
                    _M_insert_bucket_begin(_M_bucket_index(__s->_M_hash), __n);
                    _M_link_iter(__n);
                    ++_M_element_count;
                } else {
                    (void)emplace(*__s->_M_valptr());
                }
            }
        } catch (...) {
            clear();
            _M_deallocate_buckets();
            _M_reset_buckets();
            throw;
        }
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
        _M_adopt();
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
        if (this == &__x) return *this;
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
        _M_adopt();
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
        __builtin_memset(static_cast<void*>(_M_buckets), 0, _M_bucket_count * sizeof(_Node*));
    }

    void reserve(size_type __n) { rehash(_M_rehash_policy._M_bkt_for_elements(__n)); }

    void rehash(size_type __n) {
        if (__n > _M_bucket_count) _M_do_rehash(__n);
    }

    void swap(_Prism_hashtable& __x) noexcept {
        if (this == &__x) return;
        using std::swap;
        swap(_M_hash, __x._M_hash);
        swap(_M_eq, __x._M_eq);
        swap(_M_rehash_policy, __x._M_rehash_policy);
        swap(_M_element_count, __x._M_element_count);
        swap(_M_iter_head, __x._M_iter_head);
        swap(_M_iter_tail, __x._M_iter_tail);
        // A one-bucket table stores the chain head in the object. Swapping the
        // bucket pointer would leave it aimed at the other object.
        const bool __a1 = _M_bucket_count == 1;
        const bool __b1 = __x._M_bucket_count == 1;
        _Node* __a_heap = _M_buckets;
        _Node* __b_heap = __x._M_buckets;
        _Node* __a_head = _M_single_bucket_head;
        _Node* __b_head = __x._M_single_bucket_head;
        if (__a1 && __b1) {
            _M_single_bucket_head = __b_head;
            __x._M_single_bucket_head = __a_head;
            _M_buckets = &_M_single_bucket_head;
            __x._M_buckets = &__x._M_single_bucket_head;
        } else if (__a1) {
            // This table takes the heap array; __x becomes the one-bucket table.
            _M_buckets = __b_heap;
            _M_single_bucket_head = nullptr;
            __x._M_single_bucket_head = __a_head;
            __x._M_buckets = &__x._M_single_bucket_head;
        } else if (__b1) {
            __x._M_buckets = __a_heap;
            __x._M_single_bucket_head = nullptr;
            _M_single_bucket_head = __b_head;
            _M_buckets = &_M_single_bucket_head;
        } else {
            _M_buckets = __b_heap;
            __x._M_buckets = __a_heap;
        }
        swap(_M_bucket_count, __x._M_bucket_count);
        _M_adopt();
        __x._M_adopt();
    }

    template <typename... _Args>
    pair<iterator, bool> emplace(_Args&&... __args) {
        _Node* __n = _M_create(std::forward<_Args>(__args)...);
        const _Key& __k = _S_key(*__n->_M_valptr());
        size_t __code = _M_hash_code(__k);
        // A duplicate unique key must not rehash (iterators stay put).
        if constexpr (_Unique) {
            if (_Node* __ex = _M_find_node(_M_bucket_index(__code), __k, __code)) {
                _M_drop(__n);
                return {iterator(__ex), false};
            }
        }
        _M_prepare_insert(1);
        __n->_M_hash = __code;
        size_type __bkt = _M_bucket_index(__code);
        _Node* __after = nullptr;
        if constexpr (!_Unique) __after = _M_find_eq_newest(__bkt, __k, __code);
        _M_insert_bucket_begin(__bkt, __n);
        if (__after)
            _M_link_iter_after(__after, __n);
        else
            _M_link_iter(__n);
        ++_M_element_count;
        return {iterator(__n), true};
    }

    template <typename... _Args>
    iterator emplace_hint(const_iterator, _Args&&... __args) {
        return emplace(std::forward<_Args>(__args)...).first;
    }

    pair<iterator, bool> insert(const value_type& __x) {
        // const insert looks the key up first (libstdc++ _M_insert). An existing
        // unique key is not copied into a node.
        if constexpr (_Unique) {
            const _Key& __k = _S_key(__x);
            size_t __code = _M_hash_code(__k);
            if (_Node* __ex = _M_find_node(_M_bucket_index(__code), __k, __code)) return {iterator(__ex), false};
        }
        return emplace(__x);
    }

    pair<iterator, bool> insert(value_type&& __x) {
        // libstdc++ unordered_map::insert(value_type&&) forwards to
        // _Hashtable::insert(_Pair&&), which emplaces: the node is built, then
        // dropped when the unique key already exists. Looking the key up first
        // skips that move and disagrees with the differential trace.
        return emplace(std::move(__x));
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
        // find() is the newest match; the range starts at the oldest so it covers the group.
        size_t __code = _M_hash_code(__k);
        iterator __f(nullptr);
        for (_Node* __p = _M_iter_head; __p; __p = __p->_M_next_iter)
            if (__p->_M_hash == __code && _M_eq(_S_key(*__p->_M_valptr()), __k)) {
                __f = iterator(__p);
                break;
            }
        if (__f == end()) return {__f, __f};
        iterator __l = __f;
        while (__l._M_node && __l._M_node->_M_next_iter && __l._M_node->_M_next_iter->_M_hash == __code &&
               _M_eq(_S_key(*__l._M_node->_M_next_iter->_M_valptr()), __k))
            ++__l;
        ++__l;
        return {__f, __l};
    }

    pair<const_iterator, const_iterator> equal_range(const key_type& __k) const {
        size_t __code = _M_hash_code(__k);
        const_iterator __f(nullptr);
        for (const _Node* __p = _M_iter_head; __p; __p = __p->_M_next_iter)
            if (__p->_M_hash == __code && _M_eq(_S_key(*__p->_M_valptr()), __k)) {
                __f = const_iterator(__p);
                break;
            }
        if (__f == end()) return {__f, __f};
        const_iterator __l = __f;
        while (__l._M_node && __l._M_node->_M_next_iter && __l._M_node->_M_next_iter->_M_hash == __code &&
               _M_eq(_S_key(*__l._M_node->_M_next_iter->_M_valptr()), __k))
            ++__l;
        ++__l;
        return {__f, __l};
    }
};

}  // namespace std

#endif  // _PRISM_MODEL_HASH_TABLE_H
