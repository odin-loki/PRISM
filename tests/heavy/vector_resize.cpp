// std::vector operational model (src/prism/pir/models/cxx/vector) against
// C++23 [sequence.reqmts] / [vector.modifiers]: resize and copy.
// See vector_contracts.cpp. At most 4 elements (size-bounded proofs).
#include <cassert>
#include <cstddef>
#include <vector>


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
