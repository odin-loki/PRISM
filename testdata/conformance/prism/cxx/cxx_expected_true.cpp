// PRISM conformance task cxx/cxx_expected_true.cpp: expected true (memsafety)
// read value() only when the expected holds a T
#include <expected>
int cxx_expected_true(int c) {
    std::expected<int, int> e(c);
    if (e.has_value()) return e.value();
    return 0;
}
