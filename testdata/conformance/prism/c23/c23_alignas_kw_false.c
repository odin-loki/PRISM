// PRISM conformance task c23/c23_alignas_kw_false.c: expected false (no-oob)
int c23_alignas_kw_false(int i) {
    alignas(16) int a[4] = {0};
    return a[i & 7];
}
