// PRISM conformance task cxx/cxx_expected_false.cpp: expected false (memsafety)
// std::expected::value() on an unexpected
#include <expected>
int cxx_expected_false(int c) {
    std::expected<int, int> e(std::unexpected<int>(c));
    return e.value();
}
