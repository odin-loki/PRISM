// PRISM conformance task cxx/cxx_format_to_true.cpp: expected true (no-oob)
#include <format>
#include <iterator>
#include <string>
int cxx_format_to_true(int s) {
    std::string out;
    std::format_to(std::back_inserter(out), "{:+}", s & 7);
    char b[3] = {};
    return b[out.size()];  // "+0".."+7" -> index 2
}
