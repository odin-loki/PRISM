// PRISM conformance task ptrparam/ptrparam1_true.c: expected true (memsafety)
// pointer parameter: Law 6 requires NEEDS-HARNESS, never a proof or a refutation
int ptrparam1_true(const int *a, int n) {
    int s = 0;
    if (!a || n < 0 || n > 4) return 0;
    for (int i = 0; i < n; i++) s += a[i] & 1;
    return s;
}
