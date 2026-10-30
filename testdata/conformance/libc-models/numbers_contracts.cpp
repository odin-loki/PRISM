// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
// Names <numbers> so the C++ model-header conformance case sees this
// include. The scored check is
// testdata/conformance/prism/cxx/cxx_numbers_true.cpp.
// There is no false twin: a comparison against these literals is not a
// defect this header plants.
#include <numbers>

int numbers_bounds_true() {
    const auto pi = std::numbers::pi<long double>;
    const auto e = std::numbers::e<long double>;
    if (!(pi > 3 && pi < 4)) return 1;
    if (!(e > 2 && e < 3)) return 1;
    return 0;
}
