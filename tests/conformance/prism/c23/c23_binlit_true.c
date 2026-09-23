// PRISM conformance task c23/c23_binlit_true.c: expected true (no-overflow)
int c23_binlit_true(int a) {
    if (a < 0 || a > 0b1111) return 0;
    return a * 0b1000000000000000000000000;
}
