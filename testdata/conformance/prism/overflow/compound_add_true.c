// PRISM conformance task overflow/compound_add_true.c: expected true (no-overflow)
int compound_add_true(int a, int b) {
    if (a < 0 || a > 1000000000 || b < 0 || b > 1000000000) return 0;
    a += b;
    return a;
}
