// PRISM conformance task cxx/cxx_bit_false.cpp: expected false (memsafety)
// bit_width(16) is 5, one past the end of a four-int buffer
#include <bit>
int cxx_bit_false(int n) {
    int buf[4] = {0, 1, 2, 3};
    if (n < 0 || n > 16) return 0;
    return buf[std::bit_width(static_cast<unsigned>(n))];
}
