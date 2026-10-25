// PRISM conformance task div/div_ternary_false.c: expected false (no-div0)
int div_ternary_false(int a, int b) {
    int d = (b >= 0) ? b : 1;
    return a / d;
}
