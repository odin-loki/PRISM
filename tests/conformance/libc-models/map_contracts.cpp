// std::map / std::set operational models (src/prism/pir/models/cxx/map,
// set, prism_tree.h) against C++23 [associative.reqmts], [map], [set]. The
// pir stage puts the model headers first on the include path, so <map> and
// <set> below are the models (the report's `extra.cxx_models` says "model:
// map, set"); see docs/PIR.md "C++ library models". Each function is one
// contract; `_false` ones must fail for the class their yml names. At most 3
// elements with symbolic keys (size-bounded proofs).
#include <cassert>
#include <map>
#include <set>
#include <stdexcept>

// [associative.reqmts]: insert adds a key only if it is absent (the first
// value stays), iteration visits the keys in strictly increasing order.
int map_insert_true(int a, int b, int c) {
    std::map<int, int> m;
    bool ia = m.insert({a, 1}).second;
    bool ib = m.insert({b, 2}).second;
    bool ic = m.insert({c, 3}).second;
    assert(ia);
    assert(ib == (b != a));
    assert(ic == (c != a && c != b));
    assert(m.size() == 1u + (b != a) + (c != a && c != b));
    assert(m.find(a)->second == 1);
    int prev = 0;
    bool first = true;
    for (auto& [k, v] : m) {
        assert(first || prev < k);
        prev = k;
        first = false;
    }
    return 0;
}

// operator[] value-initialises a missing mapped value and returns the stored
// one otherwise; find() is end() exactly for absent keys.
int map_index_true(int a, int b) {
    std::map<int, int> m;
    m[a] = 5;
    int x = m[b];
    assert(x == (a == b ? 5 : 0));
    assert(m.size() == (a == b ? 1u : 2u));
    assert((m.find(b + 1) == m.end()) == (b + 1 != a));
    return 0;
}

// erase(key) returns the number erased; iterators to other elements stay
// valid across insertions and erasures ([associative.reqmts.general]p9).
int map_erase_true(int a, int k) {
    std::map<int, int> m{{10, 1}, {20, 2}};
    auto it = m.find(10);
    m[a] = 3;
    if (k != 10) {
        auto n = m.erase(k);
        assert(n == ((k == 20 || k == a) ? 1u : 0u));
    }
    assert(it->first == 10);
    return 0;
}

// map::at throws out_of_range exactly when the key is absent.
int map_at_true(int k) {
    std::map<int, int> m{{1, 10}, {2, 20}};
    bool threw = false;
    try {
        (void)m.at(k);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw == (k != 1 && k != 2));
    return 0;
}

// An iterator to an erased element dangles: MEM-UAF.
int map_erase_uaf_false(int k) {
    std::map<int, int> m{{1, 10}, {2, 20}};
    auto it = m.find(1);
    m.erase(k);
    return it->second;
}

// Dereferencing find() of an absent key (end()): precondition FUNC-CONTRACT.
int map_end_deref_false(int k) {
    std::map<int, int> m{{1, 10}};
    return m.find(k)->second;
}

// A wrong contract: begin() is the smallest key, not the largest.
int map_order_false(int a, int b) {
    std::map<int, int> m{{a, 0}, {b, 0}};
    assert(m.begin()->first >= b);
    return 0;
}

// [set]: unique sorted keys; lower_bound/upper_bound/count agree.
int set_order_true(int a, int b, int c) {
    std::set<int> s{a, b, c};
    assert(s.size() == 1u + (b != a) + (c != a && c != b));
    assert(s.count(a) == 1 && s.contains(b) && s.contains(c));
    assert(*s.begin() <= a && *s.rbegin() >= a);
    auto lo = s.lower_bound(b);
    assert(*lo == b);
    auto hi = s.upper_bound(b);
    assert(hi == s.end() || *hi > b);
    return 0;
}

// [multiset]: equivalent keys are all kept; erase(key) removes them all.
int multiset_count_true(int a, int b) {
    std::multiset<int> s{a, b, a};
    assert(s.size() == 3);
    assert(s.count(a) == (a == b ? 3u : 2u));
    assert(s.erase(a) == (a == b ? 3u : 2u));
    assert(s.size() == (a == b ? 0u : 1u));
    return 0;
}

// --begin(): precondition FUNC-CONTRACT.
int set_decrement_begin_false(int a) {
    std::set<int> s{a};
    auto it = s.begin();
    --it;
    return 0;
}
