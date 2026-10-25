// PRISM conformance task ptrparam/ptrparam2_true.c: expected true (no-null-deref)
// pointer parameter: Law 6 requires NEEDS-HARNESS, never a proof or a refutation
void ptrparam2_true(int *out, int v) {
    if (out) *out = v;
}
