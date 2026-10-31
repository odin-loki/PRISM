// PRISM conformance task c99/c99_hexfloat_true.c: expected true (no-fp-cast)
int c99_hexfloat_true(int x) {
    double d = 0x1.8p1 * (x & 0xFF);
    return (int)d;
}
