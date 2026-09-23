// PRISM conformance task overflow/widen_mul_false.c: expected false (no-overflow)
long long widen_mul_false(int a, int b) {
    long long r = a * b;
    return r;
}
