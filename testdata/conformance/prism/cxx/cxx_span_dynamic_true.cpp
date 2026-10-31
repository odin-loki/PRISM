// PRISM conformance task cxx/cxx_span_dynamic_true.cpp: expected true (no-oob)
// subspan indexed inside its extent
#include <span>
int cxx_span_dynamic_true(int i) {
    int a[6] = {0, 1, 2, 3, 4, 5};
    std::span<int> s(a);
    auto t = s.subspan(2, 3);
    return (i >= 0 && i < 3) ? t[static_cast<std::size_t>(i)] : 0;
}
