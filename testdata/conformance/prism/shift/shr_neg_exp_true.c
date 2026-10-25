// PRISM conformance task shift/shr_neg_exp_true.c: expected true (no-shift-ub)
int shr_neg_exp_true(int a, int s) {
    if (s < 0 || s > 31) return 0;
    return a >> s;
}
