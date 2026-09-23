// PRISM conformance task overflow/abs_manual_false.c: expected false (no-overflow)
int abs_manual_false(int a) {
    return a < 0 ? -a : a;
}
