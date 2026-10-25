// PRISM conformance task ptrparam/ptrparam2_false.c: expected false (no-null-deref)
// pointer parameter: Law 6 requires NEEDS-HARNESS, never a proof or a refutation
void ptrparam2_false(int *out, int v) {
    *out = v;
}
