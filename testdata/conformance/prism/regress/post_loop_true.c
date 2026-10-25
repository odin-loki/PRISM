// PRISM conformance task regress/post_loop_true.c: expected true (no-div0)
// regression: k-induction: the loop cut must also cover the code after the loop
int post_loop_true(int n) {
    int i = 0;
    if (n > 1000) return 0;
    while (i < n) i++;
    return 100 / (i - 5000);
}
