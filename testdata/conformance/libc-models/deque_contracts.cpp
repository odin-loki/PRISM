// std::deque operational model (src/prism/pir/models/cxx/deque) against
// C++23 [deque] / [sequence.reqmts]. The pir stage puts the model headers
// first on the include path, so <deque> below is the model (the report's
// `extra.cxx_models` says "model: deque"); see docs/finish-notes/deque.md.
// Each function is one contract; `_false` ones must fail for the class their
// yml names. At most 4 elements (size-bounded proofs).
#include <cassert>
#include <deque>

// [deque.modifiers] push at either end: size, ends, and indexing.
int deque_push_true(int x) {
    std::deque<int> d;
    d.push_back(1);
    d.push_back(x);
    d.push_front(0);
    assert(d.size() == 3);
    assert(d.front() == 0);
    assert(d.back() == x);
    assert(d[1] == 1);
    return 0;
}

// [deque.modifiers]p1: insertion at either end keeps references valid.
int deque_ref_true(int x) {
    std::deque<int> d;
    d.push_back(x);
    int& r = d.front();
    d.push_back(2);
    d.push_front(3);
    assert(r == x);
    return 0;
}

// pop_back invalidates only the erased element: the front stays.
int deque_pop_true(int x) {
    std::deque<int> d;
    d.push_back(x);
    d.push_back(5);
    d.pop_back();
    assert(d.size() == 1);
    assert(d.front() == x);
    return 0;
}

// an iterator kept across push_back is invalidated: MEM-UAF.
int deque_iter_push_uaf_false(int c) {
    std::deque<int> d;
    d.push_back(1);
    auto it = d.begin();
    d.push_back(c & 7);
    return *it;
}

// front() of an empty deque violates its precondition.
int deque_front_empty_false(int c) {
    std::deque<int> d;
    if (c) d.push_back(1);
    return d.front();
}

// operator[] past the end.
int deque_index_oob_false(unsigned i) {
    std::deque<int> d;
    d.push_back(1);
    d.push_back(2);
    return d[i & 3];
}

// push_front puts the element first, not last.
int deque_order_false(int x) {
    std::deque<int> d;
    d.push_back(1);
    d.push_front(2);
    assert(d.back() == 2);
    return x;
}
