/* PRISM conformance task fp/fp_round_true.c: expected true (no-div0) */
int fp_round_true(int x) {
    if (x < 0 || x > 100000000) return 0;
    double d = (double)x; /* exact */
    return 100 / ((int)d - x + 1);
}
