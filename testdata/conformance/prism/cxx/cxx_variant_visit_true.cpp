// PRISM conformance task cxx/cxx_variant_visit_true.cpp: expected true (no-overflow)
// std::visit over a short alternative
#include <variant>
int cxx_variant_visit_true(int x) {
    std::variant<int, short> v = static_cast<short>(x);
    return std::visit([](auto y) { return static_cast<int>(y) + 1; }, v);
}
