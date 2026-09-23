// PRISM conformance task kindmem/kindmem_alias_true.c: expected true (no-div0)
// k-induction with memory: a loop writes through a pointer into one of two arrays; the divisor is read from the other (true) or the same (false) array
int kindmem_alias_true(int n) {
    int a[4] = {1, 1, 1, 1};
    int c[4] = {1, 1, 1, 1};
    int *q = a + 1;
    if (n > 1000) n = 1000;
    for (int k = 0; k < n; k++)
        if (k == 20) q[0] = 0;
    return 100 / c[1];
}
