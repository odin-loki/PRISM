// PRISM conformance task div/mod0_true.c: expected true (no-div0)
int mod0_true(int a, int b) {
    if (b <= 0) return 0;
    return a % b;
}
