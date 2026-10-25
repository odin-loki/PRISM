// PRISM conformance task regress/kind_postloop_true.c: expected true (no-div0)
// regression: k-induction: code after the loop is checked from the havocked state
int kind_postloop_true(int n) {
    int s = 0;
    if (n > 1000) return 0;
    for (int i = 0; i < n; i++) s = 7;
    return 100 / (s - 20);
}
