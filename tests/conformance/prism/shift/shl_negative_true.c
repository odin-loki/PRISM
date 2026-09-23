// PRISM conformance task shift/shl_negative_true.c: expected true (no-shift-ub)
int shl_negative_true(int a) {
    if (a < 0 || a > 1000) return 0;
    return a << 2;
}
