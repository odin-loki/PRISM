// PRISM conformance task cxx/cxx_ratio_false.cpp: expected false (no-overflow)
// kilo::num times 3000000 overflows int
#include <ratio>
int cxx_ratio_false(int n) {
    constexpr int k = static_cast<int>(std::kilo::num);
    if (n < 0) return 0;
    return k * n;
}
