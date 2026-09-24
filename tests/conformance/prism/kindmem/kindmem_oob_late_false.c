// PRISM conformance task kindmem/kindmem_oob_late_false.c: expected false (memsafety)
// k-induction with memory: a loop writes a[k] for k < n; the write goes past the end only after the unwinding bound
int kindmem_oob_late_false(int n) {
    int a[16] = {0};
    if (n > 1000) n = 1000;
    for (int k = 0; k < n; k++) a[k] = k; /* k == 16: past the end */
    return a[0];
}
