// PRISM conformance task c23/c23_decimal_true.c: expected true (no-oob)
// C23 decimal floating point: 0.1 + 0.2 == 0.3 exactly (it does not in binary).
// clang has no _Decimal types; PRISM lowers this through models/cxx/prism_dfp.hpp.
int c23_decimal_true(int s) {
    int b[2] = {0, 0};
    _Decimal64 x = 0.1DD + 0.2DD;
    return b[x == 0.3DD ? (s & 1) : 2];
}
