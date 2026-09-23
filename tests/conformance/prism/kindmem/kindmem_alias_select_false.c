// PRISM conformance task kindmem/kindmem_alias_select_false.c: expected false (no-div0)
// k-induction with memory: a loop writes through a pointer that selects one of two arrays (target not resolved to one object)
int kindmem_alias_select_false(int n, int sel) {
    int a[4] = {1, 1, 1, 1};
    int b[4] = {1, 1, 1, 1};
    int *q = sel ? &a[1] : &b[1];
    if (n > 1000) n = 1000;
    for (int k = 0; k < n; k++)
        if (k == 20) q[0] = 0;
    return 100 / a[1];
}
