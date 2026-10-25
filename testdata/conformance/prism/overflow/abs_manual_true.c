// PRISM conformance task overflow/abs_manual_true.c: expected true (no-overflow)
int abs_manual_true(int a) {
    if (a < -2147483647) return 2147483647;
    return a < 0 ? -a : a;
}
