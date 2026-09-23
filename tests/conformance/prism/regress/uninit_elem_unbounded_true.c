// PRISM conformance task regress/uninit_elem_unbounded_true.c: expected true (memsafety)
// regression: local arrays: an unbounded (havocked) loop never un-writes an element
int uninit_elem_unbounded_true(int n, int i) {
    int a[4] = {0};
    if (n > 1000) n = 1000;
    while (n > 0) { a[n & 3] = 1; n--; }
    return a[i & 3];
}
