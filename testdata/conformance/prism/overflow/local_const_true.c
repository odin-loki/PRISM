// PRISM conformance task overflow/local_const_true.c: expected true (no-overflow)
int local_const_true(int unused) {
    int a = 2000000000;
    int b = 147483647;
    (void)unused;
    return a + b;
}
