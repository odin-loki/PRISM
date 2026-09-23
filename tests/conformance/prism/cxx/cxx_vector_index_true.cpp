// PRISM conformance task cxx/cxx_vector_index_true.cpp: expected true (no-oob)
// std::vector::operator[] one past the end (guarded)
#include <vector>
int cxx_vector_index_true(int i) {
    std::vector<int> v{1, 2, 3};
    if (i < 0 || i >= 3) return 0;
    return v[static_cast<std::size_t>(i)];
}
