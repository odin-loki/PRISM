// Trace of std::error_code. Include <system_error> and use error_code
// value(), category(), operator bool, operator==, operator!=, and clear().
// Compiled against libstdc++ and against src/prism/pir/models/cxx (that
// directory first on the include path), the lines should match.
// tests/cpp/test_cxx_system_error.cpp checks the model text; it does not
// shell out to this file.
//
// message() is not called. The model omits it so a unit that needs it fails
// to compile and pir falls back.
//
// value() is the stored int. A default error_code has value 0. category()
// compared with == is category identity: two generic codes share a category,
// two default codes share a category, and a default code is not a generic
// zero. The model returns the stored pointer. libstdc++ returns a reference
// whose == compares those objects. This trace is not PROVED and not CLEAN.
//
// Equality of error_code uses the stored int and the stored category. A
// default error_code (value 0, category id 2 = system_category in the model
// and in libstdc++) is not equal to error_code(0, generic_category()). Two
// error_code(5, generic_category()) values are equal.
//
// clear() leaves value 0, so operator bool is false. A cleared generic
// code compares equal to a default code and unequal to a generic zero.
// Its category compares equal to a default code's category and unequal
// to generic_category. The model and libstdc++ both assign system_category()
// (category id 2 and the system pointer in the model). This trace is not
// PROVED and not CLEAN.
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
    const int cat5 = (ec.category() == same.category()) ? 1 : 0;
    const int cat_def = (def.category() == def2.category()) ? 1 : 0;
    const int cat_def_generic0 = (def.category() == generic_zero.category()) ? 1 : 0;
    const int cat_diff_val = (ec.category() == std::error_code(9, std::generic_category()).category()) ? 1 : 0;

    std::error_code to_clear(5, std::generic_category());
    to_clear.clear();
    std::error_code to_clear9(9, std::generic_category());
    to_clear9.clear();
    const int clr_value = to_clear.value();
    const int clr_bool = static_cast<bool>(to_clear) ? 1 : 0;
    const int clr_eq_def = (to_clear == def) ? 1 : 0;
    const int clr_eq_generic0 = (to_clear == generic_zero) ? 1 : 0;
    const int clr_cat_def = (to_clear.category() == def.category()) ? 1 : 0;
    const int clr_cat_generic = (to_clear.category() == generic_zero.category()) ? 1 : 0;
    const int clr_cat_pair = (to_clear.category() == to_clear9.category()) ? 1 : 0;

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
    std::printf("cat5 %d\n", cat5);
    std::printf("cat_def %d\n", cat_def);
    std::printf("cat_def_generic0 %d\n", cat_def_generic0);
    std::printf("cat_diff_val %d\n", cat_diff_val);
    std::printf("clr_value %d\n", clr_value);
    std::printf("clr_bool %d\n", clr_bool);
    std::printf("clr_eq_def %d\n", clr_eq_def);
    std::printf("clr_eq_generic0 %d\n", clr_eq_generic0);
    std::printf("clr_cat_def %d\n", clr_cat_def);
    std::printf("clr_cat_generic %d\n", clr_cat_generic);
    std::printf("clr_cat_pair %d\n", clr_cat_pair);

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
    if (!cat5) return 14;
    if (!cat_def) return 15;
    if (cat_def_generic0) return 16;
    if (!cat_diff_val) return 17;
    if (def.value() != 0) return 18;
    if (clr_value != 0) return 19;
    if (clr_bool) return 20;
    if (!clr_eq_def) return 21;
    if (clr_eq_generic0) return 22;
    if (!clr_cat_def) return 23;
    if (clr_cat_generic) return 24;
    if (!clr_cat_pair) return 25;
    if (to_clear) return 26;
    if (ec.value() != 5) return 27;
    return 0;
}
