// PRISM conformance task cxx/cxx_format_exact_false.cpp: expected false (no-oob)
// Exact float text: {} of 0.1 is "0.1" (shortest round-trip), so out[2] is the digit 1 (49).
#include <format>
#include <string>
int cxx_format_exact_false(int s) {
    char b[49];
    std::string out = std::format("{}", 0.1 + (s & 0));
    return b[out[2]];
}
