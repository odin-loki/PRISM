// PRISM conformance task cxx/cxx_expected_value_true.cpp: expected true (no-overflow)
// expected value, bounded before the add
#include <expected>
std::expected<int, int> mk(int v) { if (v < 0) return std::unexpected(v); return v; }
int cxx_expected_value_true(int v) {
    auto e = mk(v);
    return (e && *e < 100) ? *e + 1 : 0;
}
