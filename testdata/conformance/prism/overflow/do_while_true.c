// PRISM conformance task overflow/do_while_true.c: expected true (no-overflow)
int do_while_true(int n) {
    int s = 0;
    if (n < 1 || n > 100) return 0;
    do { s += n; n--; } while (n > 0);
    return s;
}
