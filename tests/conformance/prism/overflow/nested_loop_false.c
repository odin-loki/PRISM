// PRISM conformance task overflow/nested_loop_false.c: expected false (no-overflow)
int nested_loop_false(int n) {
    int s = 0;
    if (n < 0 || n > 2000) return 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            s += i * j;
    return s;
}
