// PRISM conformance task c23/c23_decimal_false.c: expected false (no-oob)
int c23_decimal_false(int s) {
    int b[3] = {0, 0, 0};
    _Decimal32 q = (_Decimal32)s / 3.DF * 3.DF;  // decimal rounding: 1/3*3 != 1
    return b[(int)q];  // s = 3 -> index 3
}
