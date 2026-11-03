// PRISM conformance task cxx/cxx_numeric_false.cpp: expected false (no-overflow)
// accumulate of 1 and INT_MAX overflows int
#include <climits>
#include <numeric>
int cxx_numeric_false(int n) {
    if (n < 0 || n > 1) return 0;
    const int a[2] = {n, INT_MAX};
    return std::accumulate(a, a + 2, 0);
}
