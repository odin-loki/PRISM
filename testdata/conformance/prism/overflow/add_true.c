// PRISM conformance task overflow/add_true.c: expected true (no-overflow)
int add_true(int a, int b) {
    if (a > 1000000 || a < -1000000) return 0;
    if (b > 1000000 || b < -1000000) return 0;
    return a + b;
}
