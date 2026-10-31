// PRISM conformance task c23/c23_bitint_ops_true.c: expected true (no-overflow)
int c23_bitint_ops_true(int x) {
    _BitInt(12) a = (_BitInt(12))(x & 0x3F);
    _BitInt(12) b = a * 31;
    return (int)b;
}
