// PRISM conformance task cxx/cxx_format_string_true.cpp: expected true (no-oob)
#include <format>
#include <string>
int cxx_format_string_true(int s) {
    std::string t = std::format("{:*^9}|{:#x}|{:>4}", s & 7, 255, "ab");
    char b[32] = {};
    for (unsigned i = 0; i < t.size() && i < sizeof b; ++i) b[i] = t[i];
    return b[t.size() - 1];  // "***k*****|0xff|  ab" is 19 chars
}
