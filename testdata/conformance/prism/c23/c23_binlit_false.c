// PRISM conformance task c23/c23_binlit_false.c: expected false (no-overflow)
int c23_binlit_false(int a) {
    if (a < 0 || a > 0b11111111) return 0;
    return a * 0b1000000000000000000000000;
}
