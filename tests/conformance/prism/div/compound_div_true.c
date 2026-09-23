// PRISM conformance task div/compound_div_true.c: expected true (no-div0)
int compound_div_true(int a, int b) {
    if (b < 1) b = 1;
    a /= b;
    return a;
}
