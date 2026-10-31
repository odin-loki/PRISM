// PRISM conformance task c99/c99_volatile_true.c: expected true (no-div0)
int c99_volatile_true(int x) {
    volatile int d = (x & 7) + 1;
    return 100 / d;
}
