// PRISM conformance task div/div_loop_false.c: expected false (no-div0)
int div_loop_false(int n) {
    int s = 0;
    if (n < 1 || n > 100) return 0;
    for (int i = 0; i <= n; i++) s += 1000 / (n - i);
    return s;
}
