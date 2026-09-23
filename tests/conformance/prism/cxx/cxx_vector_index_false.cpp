// PRISM conformance task cxx/cxx_vector_index_false.cpp: expected false (no-oob)
// std::vector::operator[] one past the end
#include <vector>
int cxx_vector_index_false(int i) {
    std::vector<int> v{1, 2, 3};
    if (i < 0 || i > 3) return 0;
    return v[static_cast<std::size_t>(i)];
}
