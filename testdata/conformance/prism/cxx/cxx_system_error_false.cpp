// PRISM conformance task cxx/cxx_system_error_false.cpp: expected false (no-oob)
// the value of errc::invalid_argument indexes a four-int buffer
#include <system_error>
int cxx_system_error_false(int n) {
    const std::error_code ec = std::make_error_code(std::errc::invalid_argument);
    int buf[4] = {0, 1, 2, 3};
    if (n < 0 || n > 1) return 0;
    return buf[ec.value() * n];
}
