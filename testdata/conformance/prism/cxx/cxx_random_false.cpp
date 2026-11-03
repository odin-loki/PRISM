// PRISM conformance task cxx/cxx_random_false.cpp: expected false (no-overflow)
// the first minstd_rand value 48271 times 100000 overflows int
#include <random>
int cxx_random_false(int n) {
    std::minstd_rand g(1);
    const int v = static_cast<int>(g());
    if (n < 0) return 0;
    return v * n;
}
