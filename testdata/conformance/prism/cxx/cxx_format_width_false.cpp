// PRISM conformance task cxx/cxx_format_width_false.cpp: expected false (no-oob)
#include <format>
#include <string>
int cxx_format_width_false(int s) {
    char b[8];
    std::string t = std::format("{:08}", s);
    return b[t.size()];  // t.size() >= 8 always
}
