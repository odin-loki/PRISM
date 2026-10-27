// PRISM conformance task cxx/cxx_array_false.cpp: expected false (no-oob)
// std::array::operator[] one past the end (__n < this->size())
#include <array>
int cxx_array_false(int i) {
    std::array<int, 4> a{1, 2, 3, 4};
    if (i < 0 || i > 4) return 0;
    return a[static_cast<std::size_t>(i)];
}
