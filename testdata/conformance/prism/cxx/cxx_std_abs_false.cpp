// PRISM conformance task cxx/cxx_std_abs_false.cpp: expected false (no-overflow)
#include <cstdlib>
int cxx_std_abs_false(int a) {
    return std::abs(a) + 0;
}
