// PRISM conformance task cxx/cxx_format_range_true.cpp: expected true (no-oob)
#include <format>
#include <vector>
int cxx_format_range_true(int x) {
    std::vector<int> v{1, x & 7};
    char b[8];
    auto r = std::format_to_n(b, sizeof b, "{}", v);  // "[1, d]" is 6 chars
    return b[r.size - 1];
}
