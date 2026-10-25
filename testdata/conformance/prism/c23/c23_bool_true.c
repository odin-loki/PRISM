// PRISM conformance task c23/c23_bool_true.c: expected true (no-overflow)
int c23_bool_true(int a, bool neg) {
    if (a < -1000 || a > 1000) return 0;
    return neg ? -a : a;
}
