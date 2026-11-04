// PRISM conformance task cxx/cxx_format_range_false.cpp: expected false (no-oob)
#include <format>
#include <vector>
int cxx_format_range_false(int x) {
    std::vector<int> v{1, x};
    char b[8];
    auto r = std::format_to_n(b, sizeof b, "{}", v);
    return b[r.size - 1];
}
