// PRISM conformance task regress/loop_early_exit_true.c: expected true (no-div0)
// regression: loops: an iteration that exits early is kept, not dropped
int loop_early_exit_true(int n) {
    int i;
    if (n < 0 || n > 100) return 0;
    for (i = 0; i < n && i < 5; i++) { }
    return 100 / (n + 1);
}
