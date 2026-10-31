// PRISM conformance task cxx/cxx_optional_value_or_false.cpp: expected false (no-overflow)
// value_or then overflow
#include <optional>
int cxx_optional_value_or_false(int x) {
    std::optional<int> o;
    if (x > 0) o = x;
    return o.value_or(1) * 2;
}
