// PRISM conformance task shift/shl_overflow_true.c: expected true (no-shift-ub)
int shl_overflow_true(int a) {
    if (a < 0 || a > 1) return 0;
    return a << 30;
}
