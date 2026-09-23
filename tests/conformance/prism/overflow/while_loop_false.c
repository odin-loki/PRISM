// PRISM conformance task overflow/while_loop_false.c: expected false (no-overflow)
int while_loop_false(int n) {
    int i = 0, s = 2147483600;
    if (n < 0 || n > 100) return 0;
    while (i < n) { s += 3; i++; }
    return s;
}
