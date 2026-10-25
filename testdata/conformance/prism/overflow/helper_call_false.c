// PRISM conformance task overflow/helper_call_false.c: expected false (no-overflow)
static int twice(int x) { return x + x; }
int helper_call_false(int a) {
    if (a < 0) return 0;
    return twice(a);
}
