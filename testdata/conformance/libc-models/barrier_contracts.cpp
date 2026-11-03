// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
// Names <barrier> so the C++ model-header conformance case sees this
// include. The scored tasks are
// testdata/conformance/prism/cxx/cxx_barrier_true.cpp and
// cxx_barrier_false.cpp.
#include <barrier>

// Arrive exactly the expected count. Does not call wait.
int barrier_arrive_exact_true(int n) {
    if (n <= 0) return 0;
    std::barrier b(n);
    b.arrive(n);
    return 0;
}
