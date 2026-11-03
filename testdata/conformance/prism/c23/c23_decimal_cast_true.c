// PRISM conformance task c23/c23_decimal_cast_true.c: expected true (no-fp-cast)
// 1000 * (s & 7) is at most 7000: in int range
int c23_decimal_cast_true(int s) {
    _Decimal64 big = 1e3DD * (_Decimal64)(s & 7);
    return (int)big;
}
