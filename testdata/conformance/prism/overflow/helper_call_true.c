// PRISM conformance task overflow/helper_call_true.c: expected true (no-overflow)
static int twice(int x) { return x + x; }
int helper_call_true(int a) {
    if (a > 1000 || a < -1000) return 0;
    return twice(a) + 1;
}
