// PRISM conformance task c99/c99_hexfloat_false.c: expected false (no-fp-cast)
int c99_hexfloat_false(int x) {
    double d = 0x1.0p40 * (double)(x & 0xFF);
    return (int)d;
}
