// PRISM conformance task shift/shr_negative_false.c: expected false (no-shift-ub)
int shr_negative_false(int a) {
    return a >> (a & 63);
}
