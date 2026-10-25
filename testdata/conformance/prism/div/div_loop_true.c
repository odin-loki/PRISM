// PRISM conformance task div/div_loop_true.c: expected true (no-div0)
int div_loop_true(int n) {
    int s = 0;
    if (n < 1 || n > 100) return 0;
    for (int i = 1; i <= n; i++) s += 1000 / i;
    return s;
}
