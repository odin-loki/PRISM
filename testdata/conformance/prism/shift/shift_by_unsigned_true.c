// PRISM conformance task shift/shift_by_unsigned_true.c: expected true (no-shift-ub)
int shift_by_unsigned_true(int a, unsigned s) {
    if (s < 1 || s > 7) return 0;
    return (a >> s) / 2;
}
