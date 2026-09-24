// PRISM conformance task cxx/cxx_vector_invalidate_true.cpp: expected true (memsafety)
// reference kept across a reallocating push_back (iterator invalidation) (guarded)
#include <vector>
int cxx_vector_invalidate_true(int c) {
    std::vector<int> v;
    v.reserve(2);
    v.push_back(1);
    int &r = v[0];
    v.push_back(c & 7);  // within the reserved capacity: r stays valid
    return r + v[1];
}
