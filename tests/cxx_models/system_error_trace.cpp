// Trace of std::error_code. Include <system_error> and use only error_code
// value() and operator bool. Compiled against libstdc++ and against
// src/prism/pir/models/cxx (that directory first on the include path), the
// lines should match. tests/cpp/test_cxx_system_error.cpp checks the model
// text; it does not shell out to this file.
//
// message() is not called. The model omits it so a unit that needs it fails
// to compile and pir falls back.
#include <cstdio>
#include <system_error>

int main() {
    std::error_code ec(5, std::generic_category());
    std::error_code def;

    const int value = ec.value();
    const int engaged = static_cast<bool>(ec) ? 1 : 0;
    const int def_value = def.value();
    const int def_engaged = static_cast<bool>(def) ? 1 : 0;

    std::printf("value %d\n", value);
    std::printf("bool %d\n", engaged);
    std::printf("def %d\n", def_value);
    std::printf("def_bool %d\n", def_engaged);

    if (value != 5) return 1;
    if (!ec) return 2;
    if (def) return 3;
    if (def_value != 0) return 4;
    return 0;
}
