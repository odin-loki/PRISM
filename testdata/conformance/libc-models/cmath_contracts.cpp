// Names <cmath> so the C++ model-header conformance case sees this include.
// The scored tasks are testdata/conformance/prism/cxx/cxx_cmath_true.cpp and
// cxx_cmath_false.cpp. sin, cos, tan, exp, log, and pow are not called:
// this header does not declare them.
#include <cmath>

int cmath_sqrt0_true(int) {
    return std::sqrt(0.0) == 0.0 ? 0 : 1;
}

int cmath_fabs_true(int) {
    return std::fabs(-2.0) == 2.0 ? 0 : 1;
}

int cmath_sqrt_neg_false(int x) {
    if (x >= 0) return 0;
    std::sqrt(static_cast<double>(x));
    return 0;
}
