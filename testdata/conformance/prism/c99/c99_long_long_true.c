// PRISM conformance task c99/c99_long_long_true.c: expected true (no-overflow)
int c99_long_long_true(int x) {
    long long v = (long long)(x & 0x7FFFFFFF);
    return (int)((v * v) >> 32);
}
