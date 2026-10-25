// PRISM conformance task shift/shl_one31_false.c: expected false (no-shift-ub)
// 1 << 31 is not representable in int: undefined in C
int shl_one31_false(int s) {
    if (s < 0 || s > 31) return 0;
    return 1 << s;
}
