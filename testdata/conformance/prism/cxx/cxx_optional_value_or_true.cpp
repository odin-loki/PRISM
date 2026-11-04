// PRISM conformance task cxx/cxx_optional_value_or_true.cpp: expected true (no-overflow)
// value_or of a short: doubling fits in int
#include <optional>
int cxx_optional_value_or_true(short x) {
    std::optional<int> o;
    if (x > 0) o = x;
    return o.value_or(1) * 2;
}
