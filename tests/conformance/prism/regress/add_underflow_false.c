// PRISM conformance task regress/add_underflow_false.c: expected false (no-overflow)
// regression: S1: signed + overflows downward too
int add_underflow_false(int a, int b) {
    if (a > 0) return 0;
    return a + b;
}
