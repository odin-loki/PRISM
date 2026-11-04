// PRISM conformance task cxx/cxx_error_code_default_false.cpp: expected false (no-overflow)
// a default error_code equals (0, system_category()), so x + 1 is reached for every x
#include <system_error>
int cxx_error_code_default_false(int x) {
    std::error_code d;
    if (d == std::error_code(0, std::system_category())) return x + 1;
    return 0;
}
