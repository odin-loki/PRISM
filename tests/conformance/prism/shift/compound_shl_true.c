// PRISM conformance task shift/compound_shl_true.c: expected true (no-shift-ub)
unsigned compound_shl_true(unsigned a, int s) {
    if (s < 0 || s > 31) return a;
    a <<= s;
    return a;
}
