// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
// Names <random> so the C++ model-header conformance case sees this include.
// The scored check is testdata/conformance/prism/cxx/cxx_random_true.cpp.
// There is no false twin: random_device::operator() may be nondeterministic.
// It is not a measure of entropy, and a program that only calls it is not a
// defect. random_device is not a proof.
#include <random>

int random_minstd_seed_true(int c) {
    if (c < 0) return 0;
    std::minstd_rand a;
    a.seed(1);
    if (a() != 48271u) return 1;
    std::minstd_rand b;
    b.seed(1);
    std::minstd_rand twin;
    twin.seed(1);
    b.discard(0);
    if (b() != twin()) return 1;
    return 0;
}
