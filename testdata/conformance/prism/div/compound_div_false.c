// PRISM conformance task div/compound_div_false.c: expected false (no-div0)
int compound_div_false(int a, int b) {
    if (b < 0) b = 1;
    a %= b;
    return a;
}
