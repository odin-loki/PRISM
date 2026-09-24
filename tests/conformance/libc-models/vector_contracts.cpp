// std::vector operational model (src/prism/pir/models/cxx/vector) against
// C++23 [vector] / [sequence.reqmts]: growth and element access. The pir
// stage puts the model headers first on the include path, so <vector> below
// is the model (the report's `extra.cxx_models` says "model: vector"); see
// docs/PIR.md "C++ library models". Each function is one contract; `_false`
// ones must fail for the class their yml names. At most 4 elements
// (size-bounded proofs); vector_modifiers.cpp has insert/erase/resize.
#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <vector>

// [sequence.reqmts] push_back appends: size grows by one, back() is the
// value, earlier elements are unchanged across the two reallocations.
int vector_push_true(int x) {
    std::vector<int> v;
    v.push_back(0);
    v.push_back(1);
    v.push_back(2);
    v.push_back(x);
    assert(v.size() == 4);
    assert(v.back() == x);
    assert(v[0] == 0 && v[1] == 1 && v[2] == 2);
    assert(v.capacity() >= v.size());
    return 0;
}

// operator[] at size(): past the end (libstdc++ precondition __n < size()).
int vector_index_oob_false(int n) {
    if (n < 0 || n > 3) return 0;
    std::vector<int> v(static_cast<std::size_t>(n), 7);
    return v[v.size()];
}

// [vector.capacity]: after reserve(n), capacity() >= n and insertions do not
// reallocate while size() stays within the capacity (data() is stable).
int vector_reserve_true(int x) {
    std::vector<int> v;
    v.reserve(3);
    assert(v.capacity() >= 3);
    v.push_back(x);
    const int* p = v.data();
    v.push_back(1);
    v.push_back(2);
    assert(v.data() == p);
    return *p;
}

// A pointer kept across a push_back past capacity() dangles
// ([vector.modifiers]p1: reallocation invalidates all references).
int vector_realloc_uaf_false(int c) {
    std::vector<int> v;
    v.reserve(1);
    v.push_back(c & 7);
    const int* p = v.data();
    v.push_back(1);
    return *p;
}

// pop_back() on an empty vector (precondition !empty()).
int vector_pop_empty_false(int c) {
    std::vector<int> v;
    if (c) v.push_back(1);
    v.pop_back();
    return 0;
}

// front() on an empty vector (precondition !empty()).
int vector_front_empty_false(int c) {
    std::vector<int> v;
    if (c) v.push_back(1);
    return v.front();
}

// [sequence.reqmts] at(n) throws out_of_range iff n >= size().
int vector_at_true(int i) {
    if (i < 0 || i > 5) return 0;
    std::vector<int> v{1, 2, 3, 4};
    bool threw = false;
    try {
        (void)v.at(static_cast<std::size_t>(i));
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw == (i >= 4));
    return 0;
}

// Wrong contract: at() never throws.
int vector_at_nothrow_false(int i) {
    if (i < 0 || i > 5) return 0;
    std::vector<int> v{1, 2, 3, 4};
    bool threw = false;
    try {
        (void)v.at(static_cast<std::size_t>(i));
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(!threw);
    return 0;
}
