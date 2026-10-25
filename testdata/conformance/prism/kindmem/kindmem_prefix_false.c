// PRISM conformance task kindmem/kindmem_prefix_false.c: expected false (memsafety)
// k-induction with memory: a loop writes the prefix a[0..n) of a 64-element array; the read after it may be beyond the prefix
int kindmem_prefix_false(int n, int i) {
    int a[64];
    if (n < 20 || n > 64) return 0;
    for (int k = 0; k < n; k++) a[k] = k;
    if (i < 0 || i >= 64) return 0;
    return a[i]; /* uninitialised when i >= n */
}
