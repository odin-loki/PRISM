// PRISM conformance task cxx/cxx_format_to_false.cpp: expected false (no-oob)
#include <format>
#include <iterator>
#include <string>
int cxx_format_to_false(int s) {
    std::string out;
    std::format_to(std::back_inserter(out), "{:+}", s);
    char b[2];
    return b[out.size()];  // "+0" -> index 2
}
