// PRISM conformance task c23/c23_alignas_kw_true.c: expected true (no-oob)
int c23_alignas_kw_true(int i) {
    alignas(16) int a[4] = {0};
    return a[i & 3];
}
