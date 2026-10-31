// PRISM conformance task cxx/cxx_print_true.cpp: expected true (no-overflow)
// std::print does not hide the arithmetic
#include <print>
int cxx_print_true(short x) {
    std::print("{}\n", x);
    return x + 1;
}
