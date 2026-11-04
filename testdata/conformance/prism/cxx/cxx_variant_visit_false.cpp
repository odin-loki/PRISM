// PRISM conformance task cxx/cxx_variant_visit_false.cpp: expected false (no-overflow)
// std::visit over the int alternative: + 1 overflows
#include <variant>
int cxx_variant_visit_false(int x) {
    std::variant<int, short> v = x;
    return std::visit([](auto y) { return static_cast<int>(y) + 1; }, v);
}
