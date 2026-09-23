// PRISM conformance task memory/mem_span_false.cpp: expected false (memsafety)
#include <span>
int mem_span_false(int i) {
    int a[4] = {1, 2, 3, 4};
    std::span<int> s(a);
    return (i >= 0 && i <= 4) ? s[i] : 0;
}
