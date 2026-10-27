// PRISM conformance task cxx/cxx_charconv_true.cpp: expected true (no-overflow)
// Parses a small integer and checks the value.
#include <charconv>
int cxx_charconv_true(int x) {
    const char* s = "42";
    int v = 0;
    auto r = std::from_chars(s, s + 2, v);
    if (x < 0 || r.ec != std::errc{} || r.ptr != s + 2 || v != 42) return 0;
    return v;
}
