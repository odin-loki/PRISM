// PRISM conformance task shift/shl_negative_false.c: expected false (no-shift-ub)
// left shift of a negative value is undefined in C
int shl_negative_false(int a) {
    if (a < -1000 || a > 1000) return 0;
    return a << 2;
}
