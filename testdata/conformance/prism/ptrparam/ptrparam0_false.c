// PRISM conformance task ptrparam/ptrparam0_false.c: expected false (no-null-deref)
// pointer parameter: Law 6 requires NEEDS-HARNESS, never a proof or a refutation
int ptrparam0_false(int *p) {
    return *p;
}
