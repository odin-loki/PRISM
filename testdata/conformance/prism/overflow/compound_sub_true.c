// PRISM conformance task overflow/compound_sub_true.c: expected true (no-overflow)
int compound_sub_true(int a, int b) {
    if (b < 0 || b > 100 || a < -1000) return 0;
    a -= b;
    return a;
}
