// PRISM conformance task overflow/loop_const_false.c: expected false (no-overflow)
int loop_const_false(int x) {
    int s = 0;
    if (x < 0) return 0;
    for (int i = 0; i < 10; i++) s += x;
    return s;
}
