// PRISM conformance task overflow/loop_const_true.c: expected true (no-overflow)
int loop_const_true(int x) {
    int s = 0;
    if (x < 0 || x > 1000) return 0;
    for (int i = 0; i < 10; i++) s += x;
    return s;
}
