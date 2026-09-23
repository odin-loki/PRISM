// PRISM conformance task cxx/cxx_vector_pop_true.cpp: expected true (memsafety)
// std::vector::pop_back() on an empty vector (guarded)
#include <vector>
int cxx_vector_pop_true(int c) {
    std::vector<int> v;
    if (c) v.push_back(1);
    if (!v.empty()) v.pop_back();
    return static_cast<int>(v.size());
}
