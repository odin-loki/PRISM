// std::map / std::set operational models (src/prism/pir/models/cxx/map,
// set, prism_tree.h) against C++23 [associative.reqmts], [map], [set]. The
// pir stage puts the model headers first on the include path, so <map> and
// <set> below are the models (the report's `extra.cxx_models` says "model:
// map, set"); see docs/PIR.md "C++ library models". Each function is one
// contract; `_false` ones must fail for the class their yml names. At most 3
// elements, at most two symbolic keys (size-bounded proofs; an erase at a
// symbolic position followed by the destructor's traversal does not finish
// in the time limit, docs/PIR.md).
#include <cassert>
#include <map>
#include <set>
#include <stdexcept>

// [associative.reqmts]: insert adds a key only if it is absent (the first
// value stays); begin() is the smallest key.
int map_insert_true(int a, int b) {
    std::map<int, int> m;
    bool ia = m.insert({a, 1}).second;
    bool ib = m.insert({b, 2}).second;
    assert(ia);
    assert(ib == (b != a));
    assert(m.size() == (a == b ? 1u : 2u));
    assert(m.find(a)->second == 1);
    assert(m.begin()->first == (a < b ? a : b));
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
    assert((m.find(b) == m.end()) == false);
    return 0;
}

// erase(key) returns the number erased; iterators to other elements stay
// valid across insertions and erasures ([associative.reqmts.general]p9).
int map_erase_true(int v) {
    std::map<int, int> m;
    m[10] = v;
    m[20] = 2;
    auto it = m.find(10);
    m[30] = 3;
    assert(m.erase(20) == 1);
    assert(m.erase(20) == 0);
    assert(m.size() == 2);
    assert(it->first == 10 && it->second == v);
    return 0;
}

// map::at throws out_of_range exactly when the key is absent.
int map_at_true(int k) {
    std::map<int, int> m;
    m[1] = 10;
    m[2] = 20;
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
int map_erase_uaf_false(int c) {
    std::map<int, int> m;
    m[1] = 10;
    m[2] = 20;
    auto it = m.find(1);
    m.erase(1);
    return it->second + (c & 1);
}

// Dereferencing find() of an absent key (end()): precondition FUNC-CONTRACT.
int map_end_deref_false(int k) {
    std::map<int, int> m;
    m[1] = 10;
    return m.find(k)->second;
}

// A wrong contract: begin() is the smallest key, not the largest.
int map_order_false(int a, int b) {
    std::map<int, int> m;
    m[a] = 0;
    m[b] = 0;
    assert(m.begin()->first >= b);
    return 0;
}

// [set]: unique sorted keys; lower_bound/upper_bound/count agree.
int set_order_true(int a, int b) {
    std::set<int> s;
    s.insert(a);
    s.insert(b);
    assert(s.size() == (a == b ? 1u : 2u));
    assert(s.count(a) == 1 && s.contains(b));
    assert(*s.begin() <= a && *s.rbegin() >= a);
    assert(*s.lower_bound(b) == b);
    auto hi = s.upper_bound(b);
    assert(hi == s.end() || *hi > b);
    return 0;
}

// [multiset]: equivalent keys are all kept, after the ones already there.
int multiset_count_true(int a, int b) {
    std::multiset<int> s;
    s.insert(a);
    s.insert(b);
    s.insert(a);
    assert(s.size() == 3);
    assert(s.count(a) == (a == b ? 3u : 2u));
    return 0;
}

// erase(key) removes every equivalent element of a multiset.
int multiset_erase_true(int v) {
    std::multiset<int> s;
    s.insert(1);
    s.insert(2);
    s.insert(1);
    assert(s.erase(1) == 2);
    assert(s.size() == 1 && *s.begin() == 2);
    return v;
}

// --begin(): precondition FUNC-CONTRACT.
int set_decrement_begin_false(int a) {
    std::set<int> s;
    s.insert(a);
    auto it = s.begin();
    --it;
    return 0;
}
