// PRISM conformance task shift/shl_exp_false.c: expected false (no-shift-ub)
int shl_exp_false(int a, int s) {
    if (a < 0 || a > 1) return 0;
    if (s < 0) return 0;
    return a << s;
}
