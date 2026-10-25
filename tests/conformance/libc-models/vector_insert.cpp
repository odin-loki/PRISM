// std::vector operational model (src/prism/pir/models/cxx/vector) against
// C++23 [sequence.reqmts] / [vector.modifiers]: insert in the middle.
// See vector_contracts.cpp. At most 4 elements (size-bounded proofs).
#include <cassert>
#include <cstddef>
#include <vector>

// Same as harness.h PRISM_INDEX_LE3 (ite chain for SymMem value sets).
#define PRISM_INDEX_LE3(i) ((i) == 0 ? 0 : (i) == 1 ? 1 : (i) == 2 ? 2 : 3)

// insert(p, x) puts x before p and returns an iterator to it; the elements
// from p on move up by one (here with a reallocation: capacity 3 -> 6).
int vector_insert_true(int i, int x) {
    if (i < 0 || i > 3) return 0;
    std::vector<int> v{10, 11, 12};
    const int pos = PRISM_INDEX_LE3(i);
    auto it = v.insert(v.begin() + pos, x);
    assert(v.size() == 4);
    assert(it == v.begin() + pos && *it == x);
    assert(v[0] == (i == 0 ? x : 10));
    assert(v[3] == (i == 3 ? x : 12));
    return 0;
}

// An iterator kept across a reallocating insert dangles (MEM-UAF).
int vector_insert_uaf_false(int i) {
    std::vector<int> v{1, 2};
    auto p = v.begin();
    v.insert(v.begin() + (i & 1), 7);  // size == capacity: reallocates
    return *p;
}
