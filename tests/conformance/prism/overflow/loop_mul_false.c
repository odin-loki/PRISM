// PRISM conformance task overflow/loop_mul_false.c: expected false (no-overflow)
int loop_mul_false(int n) {
    int p = 1;
    if (n < 0 || n > 40) return 0;
    for (int i = 0; i < n; i++) p = p * 2;
    return p;
}
