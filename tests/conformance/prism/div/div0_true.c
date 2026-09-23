// PRISM conformance task div/div0_true.c: expected true (no-div0)
int div0_true(int a, int b) {
    if (b == 0) return 0;
    if (b == -1) return 0;
    return a / b;
}
