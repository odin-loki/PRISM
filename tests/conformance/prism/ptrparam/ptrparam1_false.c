// PRISM conformance task ptrparam/ptrparam1_false.c: expected false (memsafety)
// pointer parameter: Law 6 requires NEEDS-HARNESS, never a proof or a refutation
int ptrparam1_false(const int *a, int n) {
    int s = 0;
    for (int i = 0; i <= n; i++) s += a[i] & 1;
    return s;
}
