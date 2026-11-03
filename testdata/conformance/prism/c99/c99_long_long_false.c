// PRISM conformance task c99/c99_long_long_false.c: expected false (no-overflow)
int c99_long_long_false(int x) {
    long long v = (long long)x << 32;
    return (int)((v * 4) >> 40);
}
