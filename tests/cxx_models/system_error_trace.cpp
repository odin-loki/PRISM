// Trace of std::error_code. Include <system_error> and use error_code
// value(), operator bool, operator==, and operator!=. Compiled against
// libstdc++ and against src/prism/pir/models/cxx (that directory first on
// the include path), the lines should match.
// tests/cpp/test_cxx_system_error.cpp checks the model text; it does not
// shell out to this file.
//
// message() is not called. The model omits it so a unit that needs it fails
// to compile and pir falls back.
//
// Equality uses the stored int and the stored category. A default
// error_code (value 0, category id 0 in the model; system_category in
// libstdc++) is not equal to error_code(0, generic_category()). Two
// error_code(5, generic_category()) values are equal.
#include <cstdio>
#include <system_error>

int main() {
    std::error_code ec(5, std::generic_category());
    std::error_code same(5, std::generic_category());
    std::error_code generic_zero(0, std::generic_category());
    std::error_code def;
    std::error_code def2;

    const int value = ec.value();
    const int engaged = static_cast<bool>(ec) ? 1 : 0;
    const int def_value = def.value();
    const int def_engaged = static_cast<bool>(def) ? 1 : 0;
    const int eq5 = (ec == same) ? 1 : 0;
    const int neq5 = (ec != same) ? 1 : 0;
    const int eq_def_generic0 = (def == generic_zero) ? 1 : 0;
    const int neq_def_generic0 = (def != generic_zero) ? 1 : 0;
    const int eq_generic0 = (generic_zero == std::error_code(0, std::generic_category())) ? 1 : 0;
    const int eq_def = (def == def2) ? 1 : 0;
    const int neq_val = (ec == generic_zero) ? 1 : 0;

    std::printf("value %d\n", value);
    std::printf("bool %d\n", engaged);
    std::printf("def %d\n", def_value);
    std::printf("def_bool %d\n", def_engaged);
    std::printf("eq5 %d\n", eq5);
    std::printf("neq5 %d\n", neq5);
    std::printf("eq_def_generic0 %d\n", eq_def_generic0);
    std::printf("neq_def_generic0 %d\n", neq_def_generic0);
    std::printf("eq_generic0 %d\n", eq_generic0);
    std::printf("eq_def %d\n", eq_def);
    std::printf("neq_val %d\n", neq_val);

    if (value != 5) return 1;
    if (!ec) return 2;
    if (def) return 3;
    if (def_value != 0) return 4;
    if (!eq5) return 5;
    if (neq5) return 6;
    if (eq_def_generic0) return 7;
    if (!neq_def_generic0) return 8;
    if (!eq_generic0) return 9;
    if (!eq_def) return 10;
    if (neq_val) return 11;
    if (generic_zero.value() != 0) return 12;
    if (generic_zero) return 13;
    return 0;
}
