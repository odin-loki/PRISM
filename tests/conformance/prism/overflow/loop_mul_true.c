// PRISM conformance task overflow/loop_mul_true.c: expected true (no-overflow)
int loop_mul_true(int n) {
    int p = 1;
    if (n < 0 || n > 30) return 0;
    for (int i = 0; i < n; i++) p = p * 2;
    return p;
}
