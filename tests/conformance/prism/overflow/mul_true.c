// PRISM conformance task overflow/mul_true.c: expected true (no-overflow)
int mul_true(int a, int b) {
    if (a > 46340 || a < -46340 || b > 46340 || b < -46340) return 0;
    return a * b;
}
