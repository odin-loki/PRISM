// Names <bit> so the C++ model-header conformance case sees this include.
// The scored check is testdata/conformance/prism/cxx/cxx_bit_true.cpp.
// There is no false twin: a too-wide bit_ceil is a precondition, and a
// compile error is not a defect this header scores.
#include <bit>
#include <cassert>
#include <limits>

int bit_counts_true(int) {
    assert(std::countl_zero(0u) == std::numeric_limits<unsigned>::digits);
    assert(std::countr_zero(0u) == std::numeric_limits<unsigned>::digits);
    assert(std::popcount(0xAu) == 2);
    return 0;
}
