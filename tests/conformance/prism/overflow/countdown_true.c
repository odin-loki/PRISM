// PRISM conformance task overflow/countdown_true.c: expected true (no-overflow)
int countdown_true(int n) {
    int c = 0;
    if (n > 1000) return 0;
    while (n > 0) { n -= 2; c++; }
    return c;
}
