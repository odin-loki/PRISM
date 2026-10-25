// PRISM conformance task cxx/cxx_midpoint_true.cpp: expected true (no-overflow)
#include <numeric>
int cxx_midpoint_true(int a, int b) {
    return std::midpoint(a, b);
}
