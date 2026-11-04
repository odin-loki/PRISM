// PRISM conformance task cxx/cxx_print_false.cpp: expected false (no-overflow)
// std::print does not hide the arithmetic: int input overflows
#include <print>
int cxx_print_false(int x) {
    std::print("{}\n", x);
    return x + 1;
}
