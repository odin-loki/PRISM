// PRISM conformance task div/div0_false.c: expected false (no-div0)
int div0_false(int a, int b) {
    if (b == -1) return 0;
    return a / b;
}
