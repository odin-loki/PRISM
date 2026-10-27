// PRISM conformance task cxx/cxx_array_true.cpp: expected true (no-oob)
// in-bounds std::array::operator[] and data(); both address the array object
#include <array>
int cxx_array_true(int i) {
    std::array<int, 4> a{1, 2, 3, 4};
    if (i < 0 || i >= 4) return 0;
    auto k = static_cast<std::size_t>(i);
    return a[k] + a.data()[k];
}
