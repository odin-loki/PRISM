// PRISM conformance task cxx/cxx_std_array_true.cpp: expected true (no-oob)
#include <array>
int cxx_std_array_true(int i) {
    std::array<int, 4> a{1, 2, 3, 4};
    if (i < 0 || i >= 4) return 0;
    return a[static_cast<std::size_t>(i)];
}
