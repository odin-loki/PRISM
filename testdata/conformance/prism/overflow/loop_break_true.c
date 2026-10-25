// PRISM conformance task overflow/loop_break_true.c: expected true (no-overflow)
int loop_break_true(int x) {
    int s = 0;
    for (int i = 0; i < 100000; i++) {
        if (s > 1000000) break;
        s += 1000;
    }
    return s + x % 7;
}
