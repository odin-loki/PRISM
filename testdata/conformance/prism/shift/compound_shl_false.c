// PRISM conformance task shift/compound_shl_false.c: expected false (no-shift-ub)
unsigned compound_shl_false(unsigned a, int s) {
    if (s < 0) return a;
    a <<= s;
    return a;
}
