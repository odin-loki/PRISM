// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
// Names <latch> so the C++ model-header conformance case sees this include.
// The scored tasks are testdata/conformance/prism/cxx/cxx_latch_true.cpp
// (count down to zero; try_wait is true only then) and cxx_latch_false.cpp
// (count_down past zero). wait and arrive_and_wait are not called.
#include <latch>

int latch_arrive_true(int n) {
    if (n < 1) return 0;
    std::latch g(n);
    if (g.try_wait()) return 1;
    g.count_down(n);
    return g.try_wait() ? 0 : 1;
}
