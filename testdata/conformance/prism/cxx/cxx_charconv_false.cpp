// PRISM conformance task cxx/cxx_charconv_false.cpp: expected false (no-overflow)
// Parses a number that does not fit in int and treats that as success.
#include <charconv>
int cxx_charconv_false(int x) {
    if (x != 0) return 0;
    const char* s = "2147483648";
    int v = 2147483647;
    std::from_chars(s, s + 10, v);
    return v + 1;
}
