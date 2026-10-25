// PRISM conformance task overflow/compound_add_false.c: expected false (no-overflow)
int compound_add_false(int a, int b) {
    if (a < 0 || b < 0) return 0;
    a += b;
    return a;
}
