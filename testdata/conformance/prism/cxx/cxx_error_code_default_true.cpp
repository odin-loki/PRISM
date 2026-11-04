#include <system_error>
int cxx_error_code_default_true(int x) {
    std::error_code d;
    if (d == std::error_code(0, std::system_category()) && x < 100) return x + 1;
    return 0;
}
