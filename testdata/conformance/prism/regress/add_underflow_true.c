// PRISM conformance task regress/add_underflow_true.c: expected true (no-overflow)
// regression: S1: signed + overflows downward too
int add_underflow_true(int a, int b) {
    if (a > 0 || a < -1000 || b > 0 || b < -1000) return 0;
    return a + b;
}
