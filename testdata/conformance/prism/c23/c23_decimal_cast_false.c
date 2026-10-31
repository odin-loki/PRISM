// PRISM conformance task c23/c23_decimal_cast_false.c: expected false (no-fp-cast)
int c23_decimal_cast_false(int s) {
    _Decimal64 big = 1e30DD * (_Decimal64)s;
    return (int)big;  // out of int range for s != 0
}
