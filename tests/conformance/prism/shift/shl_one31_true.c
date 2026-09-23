// PRISM conformance task shift/shl_one31_true.c: expected true (no-shift-ub)
unsigned shl_one31_true(int s) {
    if (s < 0 || s > 31) return 0;
    return 1u << s;
}
