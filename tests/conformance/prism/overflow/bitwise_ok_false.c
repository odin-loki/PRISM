// PRISM conformance task overflow/bitwise_ok_false.c: expected false (no-overflow)
int bitwise_ok_false(int a, int b) {
    return (a | 1) + (b & 0x7FFFFFFF);
}
