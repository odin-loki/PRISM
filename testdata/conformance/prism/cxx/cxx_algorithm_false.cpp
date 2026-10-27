// PRISM conformance task cxx/cxx_algorithm_false.cpp: expected false (no-overflow)
// clamp with hi < lo is a precondition. The model calls __prism_check and
// traps. It does not return v, and it does not return 0 for that range.
#include <algorithm>
int cxx_algorithm_false(int lo, int hi) {
    if (!(hi < lo)) return 0;
    return std::clamp(0, lo, hi);
}
