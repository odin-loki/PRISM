// PRISM conformance task div/div_or1_false.c: expected false (no-div0)
int div_or1_false(int a, int b) {
    if (b < 0) return 0;
    return a / (b & 1);
}
