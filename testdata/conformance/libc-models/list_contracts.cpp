// std::list operational model (src/prism/pir/models/cxx/list) against
// C++23 [list] / [sequence.reqmts]. The pir stage puts the model headers
// first on the include path, so <list> below is the model (the report's
// `extra.cxx_models` says "model: list"); see docs/PIR.md "C++ library
// models". Each function is one contract; `_false` ones must fail for the
// class their yml names. At most 4 elements (size-bounded proofs). splice
// and merge are not part of the model.
#include <cassert>
#include <list>
#include <stdexcept>

// [sequence.reqmts] push_front / push_back: size, ends, and the middle stay.
int list_push_true(int x) {
    std::list<int> l;
    l.push_back(1);
    l.push_back(x);
    l.push_front(0);
    assert(l.size() == 3);
    assert(l.front() == 0);
    assert(l.back() == x);
    auto it = l.begin();
    ++it;
    assert(*it == 1);
    return 0;
}

// erase of another element leaves iterators to the rest valid.
int list_erase_true(int v) {
    std::list<int> l;
    l.push_back(v);
    l.push_back(2);
    l.push_back(3);
    auto it = l.begin();
    auto mid = it;
    ++mid;
    l.erase(mid);
    assert(l.size() == 2);
    assert(*it == v);
    ++it;
    assert(*it == 3);
    return 0;
}

// clear() frees the node: the iterator use is MEM-UAF.
int list_clear_uaf_false(int c) {
    std::list<int> l;
    l.push_back(c & 7);
    l.push_back(1);
    auto it = l.begin();
    l.clear();
    return *it;
}

// erase of the element the iterator refers to is MEM-UAF.
int list_erase_uaf_false(int c) {
    std::list<int> l;
    l.push_back(10);
    l.push_back(20);
    auto it = l.begin();
    l.erase(it);
    return *it + (c & 1);
}

// front() on an empty list (precondition !empty()).
int list_front_empty_false(int c) {
    std::list<int> l;
    if (c) l.push_back(1);
    return l.front();
}

// pop_front() on an empty list (precondition !empty()).
int list_pop_empty_false(int c) {
    std::list<int> l;
    if (c) l.push_back(1);
    l.pop_front();
    return 0;
}

// A wrong contract: front() is not the last value pushed.
int list_order_false(int a, int b) {
    std::list<int> l;
    l.push_back(a);
    l.push_back(b);
    assert(l.front() == b);
    return 0;
}
