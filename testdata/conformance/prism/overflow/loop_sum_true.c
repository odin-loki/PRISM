// PRISM conformance task overflow/loop_sum_true.c: expected true (no-overflow)
int loop_sum_true(int n) {
    int s = 0;
    if (n < 0 || n > 1000) return 0;
    for (int i = 0; i < n; i++) s += i;
    return s;
}
