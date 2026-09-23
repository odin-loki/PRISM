// PRISM conformance task overflow/compound_mul_false.c: expected false (no-overflow)
int compound_mul_false(int a) {
    if (a < -100000 || a > 100000) return 0;
    a *= a;
    return a;
}
