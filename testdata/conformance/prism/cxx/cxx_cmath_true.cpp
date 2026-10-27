// PRISM conformance task cxx/cxx_cmath_true.cpp: expected true (no-oob)
// sqrt(0) is zero; fabs of a negative finite value is the magnitude
#include <cmath>
int cxx_cmath_true(int) {
    if (std::sqrt(0.0) != 0.0) return 1;
    if (std::fabs(-2.0) != 2.0) return 1;
    return 0;
}
