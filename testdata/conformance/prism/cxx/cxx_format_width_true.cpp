// PRISM conformance task cxx/cxx_format_width_true.cpp: expected true (no-oob)
#include <format>
#include <string>
int cxx_format_width_true(int s) {
    char b[16] = {};
    std::string t = std::format("{:08}", s);
    return b[t.size()];  // an int is at most 11 chars
}
