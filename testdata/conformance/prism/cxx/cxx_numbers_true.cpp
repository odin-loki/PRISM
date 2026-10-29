// PRISM conformance task cxx/cxx_numbers_true.cpp: expected true (no-oob).
// pi is greater than 3 and less than 4. e is greater than 2 and less than 3.
// A comparison against these literals is not a proof of a numeric identity.
#include <numbers>
int cxx_numbers_true() {
    const auto pi = std::numbers::pi_v<long double>;
    const auto e = std::numbers::e_v<long double>;
    if (!(pi > 3 && pi < 4)) return 1;
    if (!(e > 2 && e < 3)) return 1;
    return 0;
}
