// PRISM conformance task overflow/compound_sub_false.c: expected false (no-overflow)
int compound_sub_false(int a, int b) {
    if (b < 0 || b > 100) return 0;
    a -= b;
    return a;
}
