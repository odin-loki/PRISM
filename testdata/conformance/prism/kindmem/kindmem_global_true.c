// PRISM conformance task kindmem/kindmem_global_true.c: expected true (memsafety)
// k-induction with memory: a loop writes a global array with a masked index; the false mask reaches past the end after the bound
int kindmem_global_g[8];
int kindmem_global_true(int n, int i) {
    if (n > 1000) n = 1000;
    for (int k = 0; k < n; k++) kindmem_global_g[k & 7] = k;
    return kindmem_global_g[i & 7];
}
