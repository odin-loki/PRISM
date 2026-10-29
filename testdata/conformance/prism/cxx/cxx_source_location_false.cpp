// PRISM conformance task cxx/cxx_source_location_false.cpp: expected false (no-oob)
// the location does not bound the index: buf[2] on a two-int buffer
#include <source_location>
int cxx_source_location_false(int n) {
    const std::source_location loc = std::source_location::current();
    int buf[2] = {static_cast<int>(loc.line()), 0};
    if (n < 0 || n > 2) return 0;
    return buf[n];
}
