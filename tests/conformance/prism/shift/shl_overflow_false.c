// PRISM conformance task shift/shl_overflow_false.c: expected false (no-shift-ub)
// 2 << 30 == 2^31 is not representable in int (C11 6.5.7p4)
int shl_overflow_false(int a) {
    if (a < 0 || a > 4) return 0;
    return a << 30;
}
