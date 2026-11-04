// PRISM conformance task cxx/cxx_mdspan_index_true.cpp: expected true (no-oob)
// mdspan rows 0..1 of a 2x3 view
#include <mdspan>
int cxx_mdspan_index_true(int i) {
    int a[6] = {0, 1, 2, 3, 4, 5};
    std::mdspan<int, std::dextents<std::size_t, 2>> m(a, 2, 3);
    return (i >= 0 && i <= 1) ? m[static_cast<std::size_t>(i), 0] : 0;
}
