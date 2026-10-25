// PRISM conformance task shift/shift_by_unsigned_false.c: expected false (no-shift-ub)
// int >> unsigned is still an arithmetic signed shift; -8>>1 = -4, -4-INT_MAX overflows
int shift_by_unsigned_false(int a, unsigned s) {
    if (s < 1 || s > 7) return 0;
    return (a >> s) - 2147483647;
}
