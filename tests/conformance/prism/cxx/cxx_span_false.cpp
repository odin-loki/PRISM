// PRISM conformance task cxx/cxx_span_false.cpp: expected false (no-oob)
// std::span::operator[] past a subspan
#include <span>
int cxx_span_false(int i) {
    int a[4] = {1, 2, 3, 4};
    std::span<int> t(a + 1, 3);  // a subspan of a
    return (i >= 0 && i <= 3) ? t[static_cast<std::size_t>(i)] : 0;
}
