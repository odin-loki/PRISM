// PRISM conformance task regress/loop_continue_false.c: expected false (no-div0)
// regression: loops: continue skips the rest of one iteration only
int loop_continue_false(int d) {
    int s = 0;
    for (int i = 0; i < 3; i++) {
        if (i == 1) continue;
        s += 1;
    }
    return 10 / (s - 2 + d);
}
