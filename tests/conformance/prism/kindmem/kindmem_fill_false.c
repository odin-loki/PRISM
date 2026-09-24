// PRISM conformance task kindmem/kindmem_fill_false.c: expected false (memsafety)
// k-induction with memory: a loop writes only the even elements; after >= 20 iterations an odd element is read
int kindmem_fill_false(int n, int i) {
    int a[8];
    if (n < 20 || n > 1000) return 0;
    while (n > 0) { a[(n * 2) & 7] = n; n--; } /* odd elements stay uninitialised */
    return a[i & 7];
}
