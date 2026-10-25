// PRISM conformance task overflow/local_const_false.c: expected false (no-overflow)
// overflow independent of the input
int local_const_false(int unused) {
    int a = 2000000000;
    int b = 147483648;
    (void)unused;
    return a + b;
}
