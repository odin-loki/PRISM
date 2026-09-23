// PRISM conformance task regress/kind_postloop_false.c: expected false (no-div0)
// regression: k-induction: code after the loop is checked from the havocked state
int kind_postloop_false(int n) {
    int s = 0;
    if (n > 1000) return 0;
    for (int i = 0; i < n; i++) s = i;
    return 100 / (s - 20);
}
