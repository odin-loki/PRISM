// PRISM conformance task div/div_ternary_true.c: expected true (no-div0)
int div_ternary_true(int a, int b) {
    int d = (b > 0) ? b : 1;
    return a / d;
}
