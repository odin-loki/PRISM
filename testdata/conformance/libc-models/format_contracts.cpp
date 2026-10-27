// Names <format> so the C++ model-header conformance case sees this include.
// The scored tasks are testdata/conformance/prism/cxx/cxx_format_true.cpp
// and cxx_format_false.cpp. No sidecar: a libc-models task must list both
// a true and a false function with expect_class, and the planted null
// pointer is the prism/cxx false task.
#include <format>

int format_digits_true(int c) {
    if (c < 0) return 0;
    char buf[8] = {};
    auto r = std::format_to_n(buf, sizeof buf, "{}", 42);
    if (r.size != 2 || buf[0] != '4' || buf[1] != '2') return 1;
    return 0;
}
