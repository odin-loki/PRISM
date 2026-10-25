// PRISM conformance task kindmem/kindmem_alias_false.c: expected false (no-div0)
// k-induction with memory: a loop writes through a pointer into one of two arrays; the divisor is read from the other (true) or the same (false) array
int kindmem_alias_false(int n) {
    int a[4] = {1, 1, 1, 1};
    int c[4] = {1, 1, 1, 1};
    int *q = c + 1; /* aliases c[1] */
    if (n > 1000) n = 1000;
    for (int k = 0; k < n; k++)
        if (k == 20) q[0] = 0;
    return 100 / c[1];
}
