// std::forward_list operational model (src/prism/pir/models/cxx/forward_list)
// against C++23 [forward.list]. The pir stage puts the model headers first
// on the include path, so <forward_list> below is the model (the report's
// `extra.cxx_models` says "model: forward_list"); see docs/PIR.md "C++ library
// models". Each function is one contract; `_false` ones must fail for the
// class their yml names. At most 4 elements (size-bounded proofs).
// splice_after and merge are not part of the model. before_begin is the
// sentinel. erase_after and pop_front free the node.
#include <cassert>
#include <forward_list>

// push_front: the last value pushed is front, and the previous one follows.
int forward_list_push_true(int x) {
    std::forward_list<int> l;
    l.push_front(1);
    l.push_front(x);
    assert(l.front() == x);
    auto it = l.begin();
    ++it;
    assert(*it == 1);
    return 0;
}

// insert_after does not invalidate an iterator to another element.
int forward_list_insert_true(int v) {
    std::forward_list<int> l;
    l.push_front(3);
    l.push_front(v);
    auto it = l.begin();
    l.insert_after(it, 2);
    assert(*it == v);
    ++it;
    assert(*it == 2);
    return 0;
}

// erase_after of another element leaves iterators to the rest valid.
int forward_list_erase_true(int v) {
    std::forward_list<int> l;
    l.push_front(3);
    l.push_front(2);
    l.push_front(v);
    auto it = l.begin();
    l.erase_after(it);
    assert(*it == v);
    ++it;
    assert(*it == 3);
    return 0;
}

// clear() frees the node: the iterator use is MEM-UAF.
int forward_list_clear_uaf_false(int c) {
    std::forward_list<int> l;
    l.push_front(1);
    l.push_front(c & 7);
    auto it = l.begin();
    l.clear();
    return *it;
}

// erase_after of the element the iterator refers to is MEM-UAF.
int forward_list_erase_uaf_false(int c) {
    std::forward_list<int> l;
    l.push_front(20);
    l.push_front(10);
    auto it = l.begin();
    l.erase_after(l.before_begin());
    return *it + (c & 1);
}

// pop_front() frees the front node: the iterator use is MEM-UAF.
int forward_list_pop_uaf_false(int c) {
    std::forward_list<int> l;
    l.push_front(20);
    l.push_front(10);
    auto it = l.begin();
    l.pop_front();
    return *it + (c & 1);
}

// front() on an empty list (precondition !empty()).
int forward_list_front_empty_false(int c) {
    std::forward_list<int> l;
    if (c) l.push_front(1);
    return l.front();
}

// pop_front() on an empty list (precondition !empty()).
int forward_list_pop_empty_false(int c) {
    std::forward_list<int> l;
    if (c) l.push_front(1);
    l.pop_front();
    return 0;
}

// A wrong contract: front() is not the first value pushed.
int forward_list_order_false(int a, int b) {
    std::forward_list<int> l;
    l.push_front(a);
    l.push_front(b);
    assert(l.front() == a);
    return 0;
}
