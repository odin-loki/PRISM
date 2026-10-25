// PRISM conformance task overflow/early_return_true.c: expected true (no-overflow)
int early_return_true(int a) {
    if (a >= 2147483647 - 10) return a;
    int b = a + 10;
    return b;
}
