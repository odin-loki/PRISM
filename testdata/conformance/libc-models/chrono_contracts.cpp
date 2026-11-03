// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
// Names <chrono> so the C++ model-header conformance case sees this include.
// The scored tasks are testdata/conformance/prism/cxx/cxx_chrono_true.cpp
// and cxx_chrono_false.cpp.
#include <chrono>

int chrono_add_small_true(int c) {
    if (c < 0) return 0;
    std::chrono::duration<long long> zero{};
    if (zero.count() != 0) return 1;
    std::chrono::duration<long long> a(2);
    std::chrono::duration<long long> b(3);
    if (a.count() != 2 || b.count() != 3) return 1;
    return (a + b).count() == 5 ? 0 : 1;
}
