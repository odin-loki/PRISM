// PRISM conformance task cxx/cxx_cmath_false.cpp: expected false (no-oob)
// std::sqrt of a negative value
#include <cmath>
int cxx_cmath_false(int x) {
    if (x >= 0) return 0;
    std::sqrt(static_cast<double>(x));
    return 0;
}
