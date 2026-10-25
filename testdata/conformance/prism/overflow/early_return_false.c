// PRISM conformance task overflow/early_return_false.c: expected false (no-overflow)
int early_return_false(int a) {
    if (a >= 2147483647 - 10) return a;
    int b = a + 12;
    return b;
}
