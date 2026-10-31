// PRISM conformance task cxx/cxx_variant_visit3_true.cpp: expected true (no-overflow)
// std::visit over three variants with bounded alternatives
#include <variant>
int cxx_variant_visit3_true(signed char x, short y, bool c) {
    std::variant<signed char, short> a(x);
    std::variant<signed char, short> b(y);
    std::variant<int, bool> d(c);
    return std::visit([](auto p, auto q, auto r) { return static_cast<int>(p) + static_cast<int>(q) + static_cast<int>(r ? 1 : 0); }, a, b, d);
}
