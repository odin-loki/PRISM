// PRISM conformance task cxx/cxx_numbers_false.cpp: expected false (no-oob)
// int(pi) is 3, one past the end of a three-int buffer
#include <numbers>
int cxx_numbers_false(int n) {
    int buf[3] = {0, 1, 2};
    if (n != 1) return 0;
    return buf[static_cast<int>(std::numbers::pi)];
}
