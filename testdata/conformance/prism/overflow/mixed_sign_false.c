// PRISM conformance task overflow/mixed_sign_false.c: expected false (no-overflow)
// unsigned char promotes to int, so the addition is signed
int mixed_sign_false(int a, unsigned char b) {
    return a + b;
}
