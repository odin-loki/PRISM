// PRISM conformance task cxx/cxx_mdspan_true.cpp: expected true (no-oob)
// in-range std::mdspan::operator(); the value is the array element
#include <mdspan>
int cxx_mdspan_true(int i) {
    int a[4] = {1, 2, 3, 4};
    std::mdspan<int> m(a, 4);
    if (i < 0 || i >= 4) return 0;
    auto k = static_cast<std::size_t>(i);
    return m(k);
}
