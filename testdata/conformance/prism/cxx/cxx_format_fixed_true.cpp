// PRISM conformance task cxx/cxx_format_fixed_true.cpp: expected true (no-oob)
#include <format>
#include <string>
int cxx_format_fixed_true(int s) {
    int b[2] = {0, 0};
    std::string a = std::format("{:.2f}", 2.675);  /* binary 2.67499.. -> "2.67" */
    std::string e = std::format("{:e}", 1234.5);    /* "1.234500e+03" */
    return b[a == "2.67" && e == "1.234500e+03" ? (s & 1) : 2];
}
