// PRISM conformance task regress/helper_early_return_true.c: expected true (no-div0)
// regression: inliner: a return in the middle of a static helper leaves it
static int nz_t(int a) { if (a == 0) return 1; return 2; }
int helper_early_return_true(int a) {
    int r = nz_t(a);
    return 10 / r;
}
