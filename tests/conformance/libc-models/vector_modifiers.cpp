// std::vector operational model (src/prism/pir/models/cxx/vector) against
// C++23 [sequence.reqmts] / [vector.modifiers]: insert, erase, resize, copy.
// See vector_contracts.cpp. At most 4 elements (size-bounded proofs).
#include <cassert>
#include <cstddef>
#include <vector>

// erase(q) removes *q; the following elements move down; the result points
// at the element after the erased one.
int vector_erase_true(int i) {
    if (i < 0 || i > 2) return 0;
    std::vector<int> v{10, 11, 12};
    auto it = v.erase(v.begin() + i);
    assert(v.size() == 2);
    assert(it == v.begin() + i);
    assert(v[0] == (i == 0 ? 11 : 10));
    assert(v[1] == (i == 2 ? 11 : 12));
    return 0;
}

// erase(end()): q must be dereferenceable ([sequence.reqmts]).
int vector_erase_end_false(int c) {
    std::vector<int> v{1, 2};
    v.erase(v.end());
    return c;
}

// insert(p, t) inserts before p and returns an iterator to the new element.
int vector_insert_true(int i, int x) {
    if (i < 0 || i > 2) return 0;
    std::vector<int> v{0, 1};
    auto it = v.insert(v.begin() + i, x);
    assert(v.size() == 3);
    assert(*it == x);
    assert(it - v.begin() == i);
    assert(v[i == 0 ? 1 : 0] == 0);
    assert(v[i == 2 ? 1 : 2] == 1);
    return 0;
}

// insert at a position of another vector (not an iterator into *this).
int vector_insert_foreign_false(int x) {
    std::vector<int> v{0, 1}, w{2, 3};
    v.insert(w.begin(), x);
    return 0;
}

// resize(n): new elements are value-initialised; shrinking keeps the prefix.
int vector_resize_true(int m) {
    if (m < 0 || m > 3) return 0;
    std::vector<int> v{1, 2};
    v.resize(static_cast<std::size_t>(m));
    assert(v.size() == static_cast<std::size_t>(m));
    if (m > 0) assert(v[0] == 1);
    if (m > 1) assert(v[1] == 2);
    if (m > 2) assert(v[2] == 0);
    return 0;
}

// Wrong contract: growing back after a shrink keeps the old value.
int vector_resize_value_false(int x) {
    std::vector<int> v{x, 2};
    v.resize(1);
    v.resize(2);
    assert(v[1] == 2);
    return 0;
}

// [container.reqmts] a copy compares equal and is independent of its source.
int vector_copy_true(int x) {
    std::vector<int> v{1, 2};
    std::vector<int> w(v);
    assert(w.size() == 2 && w[0] == 1 && w[1] == 2);
    w[0] = x;
    assert(v[0] == 1);
    return 0;
}
