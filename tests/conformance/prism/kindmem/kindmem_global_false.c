// PRISM conformance task kindmem/kindmem_global_false.c: expected false (memsafety)
// k-induction with memory: a loop writes a global array with a masked index; the false mask reaches past the end after the bound
int kindmem_global_h[8];
int kindmem_global_false(int n, int i) {
    if (n > 1000) n = 1000;
    for (int k = 0; k < n; k++) kindmem_global_h[k & 15] = k; /* k == 8: past the end */
    return kindmem_global_h[i & 7];
}
