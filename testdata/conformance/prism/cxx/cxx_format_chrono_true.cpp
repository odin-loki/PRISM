// PRISM conformance task cxx/cxx_format_chrono_true.cpp: expected true (no-oob)
#include <chrono>
#include <format>
int cxx_format_chrono_true(short s) {
    char b[16];
    std::chrono::duration<long long> d(s);
    auto r = std::format_to_n(b, sizeof b, "{}", d);
    return b[r.size - 1];  // "<count>s" fits in 16
}
