// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
// Names <numeric> so the C++ model-header conformance case sees this
// include. The scored check is
// testdata/conformance/prism/cxx/cxx_numeric_true.cpp.
// There is no false twin. A wrapped sum is not a result this header
// returns, so it is not planted as a failing task.
#include <numeric>

int numeric_midpoint_gcd_true(int a, int b) {
    if (a != 2 || b != 4) return 0;
    if (std::midpoint(a, b) != 3) return 1;
    if (std::gcd(0, 0) != 0) return 1;
    return 0;
}
