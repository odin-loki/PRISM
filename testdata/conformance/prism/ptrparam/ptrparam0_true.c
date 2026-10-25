// PRISM conformance task ptrparam/ptrparam0_true.c: expected true (no-null-deref)
// pointer parameter: Law 6 requires NEEDS-HARNESS, never a proof or a refutation
int ptrparam0_true(int *p) {
    if (!p) return 0;
    return *p;
}
