// PRISM conformance task cxx/cxx_expected_value_false.cpp: expected false (no-overflow)
// expected value then overflow
#include <expected>
std::expected<int, int> mk(int v) { if (v < 0) return std::unexpected(v); return v; }
int cxx_expected_value_false(int v) {
    auto e = mk(v);
    return e ? *e + 1 : 0;
}
