// PRISM conformance task cxx/cxx_vector_front_false.cpp: expected false (memsafety)
// std::vector::front() on an empty vector
#include <vector>
int cxx_vector_front_false(int c) {
    std::vector<int> v;
    if (c) v.push_back(c & 7);
    return v.front();
}
