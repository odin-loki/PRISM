// PRISM conformance task regress/mixed_width_true.c: expected true (no-overflow)
// regression: R1: operands of different widths never crash the encoder
int mixed_width_true(long long p0) {
    unsigned v0 = 12;
    v0 &= (p0 + (~p0));
    return (int)v0;
}
