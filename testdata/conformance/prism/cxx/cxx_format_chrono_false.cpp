// PRISM conformance task cxx/cxx_format_chrono_false.cpp: expected false (no-oob)
#include <chrono>
#include <format>
int cxx_format_chrono_false(int s) {
    char b[4];
    std::chrono::duration<long long> d(s);
    auto r = std::format_to_n(b, sizeof b, "{}", d);
    return b[r.size - 1];
}
