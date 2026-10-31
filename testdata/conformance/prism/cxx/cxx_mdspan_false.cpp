// PRISM conformance task cxx/cxx_mdspan_false.cpp: expected false (no-oob)
// std::mdspan::operator() one past the extent
#include <mdspan>
int cxx_mdspan_false(int i) {
    int a[4] = {1, 2, 3, 4};
    std::mdspan<int, std::dextents<std::size_t, 1>> m(a, 4);
    if (i < 0 || i > 4) return 0;
    return m[static_cast<std::size_t>(i)];
}
