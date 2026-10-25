// PRISM conformance task overflow/nested_loop_true.c: expected true (no-overflow)
int nested_loop_true(int n) {
    int s = 0;
    if (n < 0 || n > 50) return 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            s += i * j;
    return s;
}
