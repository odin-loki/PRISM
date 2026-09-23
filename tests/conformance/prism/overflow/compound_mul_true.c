// PRISM conformance task overflow/compound_mul_true.c: expected true (no-overflow)
int compound_mul_true(int a) {
    if (a < -1000 || a > 1000) return 0;
    a *= a;
    a *= 2;
    return a;
}
