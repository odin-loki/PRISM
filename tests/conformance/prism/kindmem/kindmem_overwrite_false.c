// PRISM conformance task kindmem/kindmem_overwrite_false.c: expected false (no-div0)
// k-induction with memory: a loop zeroes one element of a small array late; the divisor is the other (true) or the same (false) element
int kindmem_overwrite_false(int n) {
    int d[2] = {1, 1};
    if (n > 1000) n = 1000;
    for (int k = 0; k < n; k++)
        if (k == 30) d[0] = 0;
    return 100 / d[0];
}
