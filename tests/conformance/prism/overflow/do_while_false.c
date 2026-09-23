// PRISM conformance task overflow/do_while_false.c: expected false (no-overflow)
int do_while_false(int n) {
    int s = 0;
    if (n < 1) return 0;
    do { s += n; n--; } while (n > 0);
    return s;
}
