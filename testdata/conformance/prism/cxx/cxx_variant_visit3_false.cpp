// PRISM conformance task cxx/cxx_variant_visit3_false.cpp: expected false (no-overflow)
// std::visit over three variants with an int alternative: the sum overflows
#include <variant>
int cxx_variant_visit3_false(int x, short y, bool c) {
    std::variant<int, short> a(x);
    std::variant<int, short> b(y);
    std::variant<int, bool> d(c);
    return std::visit([](auto p, auto q, auto r) { return static_cast<int>(p) + static_cast<int>(q) + static_cast<int>(r ? 1 : 0); }, a, b, d);
}
