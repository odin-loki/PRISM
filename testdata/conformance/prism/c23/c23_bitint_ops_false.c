// PRISM conformance task c23/c23_bitint_ops_false.c: expected false (no-overflow)
int c23_bitint_ops_false(int x) {
    _BitInt(12) a = (_BitInt(12))(x & 0x7F);
    _BitInt(12) b = a * 31;
    return (int)b;
}
