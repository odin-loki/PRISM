// PRISM conformance task cxx/cxx_format_float_true.cpp: expected true (no-oob)
#include <format>
int cxx_format_float_true(double v) {
    char b[8];
    auto r = std::format_to_n(b, sizeof b, "{:.3f}", v);
    return static_cast<int>(r.out - b);  // never past b + 8
}
