// PRISM conformance task cxx/cxx_vector_pop_false.cpp: expected false (memsafety)
// std::vector::pop_back() on an empty vector
#include <vector>
int cxx_vector_pop_false(int c) {
    std::vector<int> v;
    if (c) v.push_back(1);
    v.pop_back();
    return static_cast<int>(v.size());
}
