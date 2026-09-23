// PRISM conformance task regress/loop_continue_true.c: expected true (no-div0)
// regression: loops: continue skips the rest of one iteration only
int loop_continue_true(int d) {
    int s = 0;
    if (d < 0 || d > 1000) return 0;
    for (int i = 0; i < 3; i++) {
        if (i == 1) continue;
        s += 1;
    }
    return 10 / (s - 1 + d);
}
