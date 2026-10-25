// PRISM conformance task div/mod0_false.c: expected false (no-div0)
int mod0_false(int a, int b) {
    if (b < 0) return 0;
    return a % b;
}
