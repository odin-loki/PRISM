// PRISM conformance task kindmem/kindmem_ptrinc_false.c: expected false (memsafety)
// k-induction with memory: a loop writes through an incremented pointer; one write too many goes past the end
int kindmem_ptrinc_false(int n) {
    int a[8] = {0};
    int *p = a;
    if (n < 0) n = 0;
    if (n > 9) n = 9;
    while (n-- > 0) *p++ = 1; /* the 9th write is past the end */
    return a[0];
}
