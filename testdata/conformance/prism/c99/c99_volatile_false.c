// PRISM conformance task c99/c99_volatile_false.c: expected false (no-div0)
int c99_volatile_false(int x) {
    volatile int d = x & 7;
    return 100 / d;
}
