// PRISM conformance task div/div_or1_true.c: expected true (no-div0)
int div_or1_true(int a, int b) {
    if (b < 0) return 0;
    return a / (b | 1);
}
