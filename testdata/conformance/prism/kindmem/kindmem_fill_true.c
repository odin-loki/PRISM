// PRISM conformance task kindmem/kindmem_fill_true.c: expected true (memsafety)
// k-induction with memory: a loop writes only the even elements; after >= 20 iterations an odd element is read
int kindmem_fill_true(int n, int i) {
    int a[8] = {0};
    if (n < 20 || n > 1000) return 0;
    while (n > 0) { a[(n * 2) & 7] = n; n--; }
    return a[i & 7];
}
