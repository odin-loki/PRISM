// PRISM conformance task overflow/widen_mul_true.c: expected true (no-overflow)
// int*int widened before the multiply can never overflow 64 bits
long long widen_mul_true(int a, int b) {
    return (long long)a * b;
}
