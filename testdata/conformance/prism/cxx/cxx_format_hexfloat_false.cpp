// PRISM conformance task cxx/cxx_format_hexfloat_false.cpp: expected false (no-oob)
// 'a' text of 12345 is "1.81c8p+13" (10 chars), past b[8]
#include <format>
#include <string>
int cxx_format_hexfloat_false(int s) {
    char b[8] = {};
    std::string t = std::format("{:La}", static_cast<double>(s));
    return b[t.size()];
}
