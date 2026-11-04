// PRISM conformance task cxx/cxx_format_hexfloat_true.cpp: expected true (no-oob)
#include <format>
#include <string>
int cxx_format_hexfloat_true(int s) {
    char b[32];
    std::string t = std::format("{:La}", static_cast<double>(s));
    if (t.size() > 22) return 1;
    b[t.size()] = 0;  // 'a' text is at most 22 chars, no "0x"
    return b[t.size()];
}
