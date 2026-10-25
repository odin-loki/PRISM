// PRISM conformance task overflow/div_intmin_false.c: expected false (no-overflow)
int div_intmin_false(int a, int b) {
    if (b == 0) return 0;
    return a / b;
}
