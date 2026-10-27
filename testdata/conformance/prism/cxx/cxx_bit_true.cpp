// PRISM conformance task cxx/cxx_bit_true.cpp: expected true (memsafety)
// countl_zero(0) is the width of unsigned. popcount of a small value is 2.
#include <bit>
#include <cassert>
#include <limits>
int cxx_bit_true(int) {
    assert(std::countl_zero(0u) == std::numeric_limits<unsigned>::digits);
    assert(std::popcount(0xAu) == 2);
    return 0;
}
