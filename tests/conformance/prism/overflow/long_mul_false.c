// PRISM conformance task overflow/long_mul_false.c: expected false (no-overflow)
long long_mul_false(long a) {
    if (a < 0) return 0;
    return a * a;
}
