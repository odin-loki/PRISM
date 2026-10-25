// PRISM conformance task cxx/cxx_std_abs_true.cpp: expected true (no-overflow)
#include <cstdlib>
int cxx_std_abs_true(int a) {
    if (a == -2147483647 - 1) return 0;
    return std::abs(a);
}
