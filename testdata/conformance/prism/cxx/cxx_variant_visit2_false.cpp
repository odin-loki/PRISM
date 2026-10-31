// PRISM conformance task cxx/cxx_variant_visit2_false.cpp: expected false (no-overflow)
// std::visit over two variants: int + int alternative overflows
#include <variant>
int cxx_variant_visit2_false(int x, int y) {
    std::variant<int, short> a(x);
    std::variant<int, short> b(y);
    return std::visit([](auto p, auto q) { return static_cast<int>(p) + static_cast<int>(q); }, a, b);
}
