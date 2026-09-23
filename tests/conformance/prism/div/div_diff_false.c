// PRISM conformance task div/div_diff_false.c: expected false (no-div0)
int div_diff_false(int a, int b) {
    if (a < 0 || a > 1000 || b < 0 || b > 1000) return 0;
    return 1000 / (a - b);
}
