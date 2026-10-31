// PRISM conformance task cxx/cxx_format_float_false.cpp: expected false (no-oob)
#include <format>
int cxx_format_float_false(double v) {
    char b[8];
    auto r = std::format_to_n(b, sizeof b, "{:e}", v);
    return b[r.size];  // size is the untruncated length, >= 8 for most v
}
