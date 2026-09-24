// std::vector operational model (src/prism/pir/models/cxx/vector) against
// C++23 [sequence.reqmts] / [vector.modifiers]: erase.
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
