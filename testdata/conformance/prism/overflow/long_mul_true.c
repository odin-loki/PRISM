// PRISM conformance task overflow/long_mul_true.c: expected true (no-overflow)
long long_mul_true(long a) {
    if (a < -3000000000L || a > 3000000000L) return 0;
    return a * a;
}
