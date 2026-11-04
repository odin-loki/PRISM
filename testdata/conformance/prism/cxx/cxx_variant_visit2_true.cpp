// PRISM conformance task cxx/cxx_variant_visit2_true.cpp: expected true (no-overflow)
// std::visit over two variants of short inputs: the sum fits in int
#include <variant>
int cxx_variant_visit2_true(short x, short y) {
    std::variant<int, short> a(x);
    std::variant<int, short> b(y);
    return std::visit([](auto p, auto q) { return static_cast<int>(p) + static_cast<int>(q); }, a, b);
}
