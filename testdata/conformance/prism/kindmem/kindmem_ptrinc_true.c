// PRISM conformance task kindmem/kindmem_ptrinc_true.c: expected true (memsafety)
// k-induction with memory: a loop writes through an incremented pointer; one write too many goes past the end
int kindmem_ptrinc_true(int n) {
    int a[8] = {0};
    int *p = a;
    if (n < 0) n = 0;
    if (n > 8) n = 8;
    while (n-- > 0) *p++ = 1;
    return a[0];
}
