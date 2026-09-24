// PRISM conformance task kindmem/kindmem_ptrload_false.c: expected false (no-div0)
// k-induction with memory: a loop writes through a pointer loaded from a table (target not resolved to one object)
int kindmem_ptrload_false(int n, int sel) {
    int a[4] = {1, 1, 1, 1};
    int c[4] = {1, 1, 1, 1};
    int *tab[2] = {a, c};
    int *q = tab[sel & 1];
    if (n > 1000) n = 1000;
    for (int k = 0; k < n; k++)
        if (k == 20) q[1] = 0;
    return 100 / c[1];
}
