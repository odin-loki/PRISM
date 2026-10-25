// PRISM conformance task overflow/loop_sum_false.c: expected false (no-overflow)
int loop_sum_false(int n) {
    int s = 0;
    if (n < 0 || n > 100000) return 0;
    for (int i = 0; i < n; i++) s += i;
    return s;
}
