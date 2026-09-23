// PRISM conformance task regress/helper_early_return_false.c: expected false (no-div0)
// regression: inliner: a return in the middle of a static helper leaves it
static int nz_f(int a) { if (a == 0) return 0; return 1; }
int helper_early_return_false(int a) {
    int r = nz_f(a);
    return 10 / r;
}
