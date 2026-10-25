// PRISM conformance task overflow/bitwise_ok_true.c: expected true (no-overflow)
int bitwise_ok_true(int a, int b) {
    return (a & 0xFF) + (b & 0xFF) + (a ^ b) % 3;
}
