// PRISM conformance task cxx/cxx_source_location_true.cpp: expected true (no-oob)
// current() reports the compiler line. That line is not 0, so the out-of-range
// index is not reached.
#include <source_location>
int cxx_source_location_true(int n) {
    const std::source_location loc = std::source_location::current();
    int a[1] = {n};
    if (loc.line() == 0) return a[1];
    return a[0];
}
