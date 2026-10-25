// PRISM conformance task overflow/while_loop_true.c: expected true (no-overflow)
int while_loop_true(int n) {
    int i = 0, s = 0;
    if (n < 0 || n > 100) return 0;
    while (i < n) { s += 3; i++; }
    return s;
}
