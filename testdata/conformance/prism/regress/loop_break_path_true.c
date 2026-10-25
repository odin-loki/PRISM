// PRISM conformance task regress/loop_break_path_true.c: expected true (no-div0)
// regression: loops: the state at a break reaches the code after the loop
int loop_break_path_true(int a) {
    int x = 2;
    for (;;) {
        if (a) break;
        x = 3;
        break;
    }
    return 10 / (x - 1);
}
