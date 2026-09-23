// PRISM conformance task overflow/ternary_false.c: expected false (no-overflow)
int ternary_false(int a) {
    int b = a > 2000 ? 2000 : a;
    return b * b * b;
}
