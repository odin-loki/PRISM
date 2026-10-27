// PRISM conformance task cxx/cxx_system_error_true.cpp: expected true (no-oob)
// error_code(5, generic_category()).value() is 5; a default error_code is false
#include <system_error>
int cxx_system_error_true(int) {
    std::error_code ec(5, std::generic_category());
    if (ec.value() != 5) return 1;
    std::error_code def;
    if (def) return 1;
    return 0;
}
