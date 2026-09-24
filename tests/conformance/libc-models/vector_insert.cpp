// std::vector operational model (src/prism/pir/models/cxx/vector) against
// C++23 [sequence.reqmts] / [vector.modifiers]: insert.
// See vector_contracts.cpp. At most 4 elements (size-bounded proofs).
#include <cassert>
#include <cstddef>
#include <vector>


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
