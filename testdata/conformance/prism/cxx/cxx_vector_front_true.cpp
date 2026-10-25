// PRISM conformance task cxx/cxx_vector_front_true.cpp: expected true (memsafety)
// std::vector::front() on an empty vector (guarded)
#include <vector>
int cxx_vector_front_true(int c) {
    std::vector<int> v;
    if (c) v.push_back(c & 7);
    return v.empty() ? 0 : v.front() + v.back();
}
