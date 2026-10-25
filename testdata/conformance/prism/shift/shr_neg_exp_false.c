// PRISM conformance task shift/shr_neg_exp_false.c: expected false (no-shift-ub)
int shr_neg_exp_false(int a, int s) {
    if (s > 31) return 0;
    return a >> s;
}
