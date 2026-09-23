// PRISM conformance task overflow/ternary_true.c: expected true (no-overflow)
int ternary_true(int a) {
    int b = a > 100 ? 100 : a;
    int c = b < -100 ? -100 : b;
    return c * c * c;
}
