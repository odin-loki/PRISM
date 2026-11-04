// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
// Names <print> so the C++ model-header conformance case sees this include.
// The scored tasks are testdata/conformance/prism/cxx/cxx_print_true.cpp
// and cxx_print_false.cpp (the planted null FILE* is the prism/cxx false task).
#include <cstdio>
#include <print>

int print_stdout_true(int c) {
    if (c < 0) return 0;
    std::println(stdout, "{}", c);
    std::print(stdout, "{} {}\n", c, c + 0L);
    return 0;
}
