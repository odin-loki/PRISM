// PRISM conformance task cxx/cxx_vector_invalidate_false.cpp: expected false (memsafety)
// reference kept across a reallocating push_back (iterator invalidation)
#include <vector>
int cxx_vector_invalidate_false(int c) {
    std::vector<int> v;
    v.push_back(1);
    int &r = v[0];
    if (c) v.push_back(2);  // reallocates: r dangles
    return r;
}
