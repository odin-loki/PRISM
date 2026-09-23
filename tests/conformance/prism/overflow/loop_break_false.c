// PRISM conformance task overflow/loop_break_false.c: expected false (no-overflow)
int loop_break_false(int x) {
    int s = x;
    for (int i = 0; i < 100; i++) {
        if (s < 0) break;
        s += 1000;
    }
    return s;
}
