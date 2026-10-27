// PRISM conformance task cxx/cxx_numeric_true.cpp: expected true (no-overflow)
// midpoint of two small ints, and gcd(0, 0) == 0.
#include <numeric>
int cxx_numeric_true(int a, int b) {
    if (a != 2 || b != 4) return 0;
    if (std::midpoint(a, b) != 3) return 1;
    if (std::gcd(0, 0) != 0) return 1;
    return 0;
}
