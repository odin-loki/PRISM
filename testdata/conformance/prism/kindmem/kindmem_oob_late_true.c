// PRISM conformance task kindmem/kindmem_oob_late_true.c: expected true (memsafety)
// k-induction with memory: a loop writes a[k] for k < n; the write goes past the end only after the unwinding bound
int kindmem_oob_late_true(int n) {
    int a[16] = {0};
    if (n > 1000) n = 1000;
    for (int k = 0; k < n && k < 16; k++) a[k] = k;
    return a[0];
}
