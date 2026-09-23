// PRISM conformance task div/div_diff_true.c: expected true (no-div0)
int div_diff_true(int a, int b) {
    if (a < 0 || a > 1000 || b < 0 || b > 1000) return 0;
    if (a == b) return 0;
    return 1000 / (a - b);
}
