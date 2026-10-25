// PRISM conformance task shift/shl_exp_true.c: expected true (no-shift-ub)
int shl_exp_true(int a, int s) {
    if (a < 0 || a > 255) return 0;
    if (s < 0 || s > 20) return 0;
    return a << s;
}
