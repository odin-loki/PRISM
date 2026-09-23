// PRISM conformance task overflow/countdown_false.c: expected false (no-overflow)
int countdown_false(int n) {
    int c = 0;
    while (n < 5) { n -= 1000000000; c++; if (c > 3) break; }
    return c;
}
