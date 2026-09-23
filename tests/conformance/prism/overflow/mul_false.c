// PRISM conformance task overflow/mul_false.c: expected false (no-overflow)
int mul_false(int a, int b) {
    if (a > 65536 || a < -65536 || b > 65536 || b < -65536) return 0;
    return a * b;
}
