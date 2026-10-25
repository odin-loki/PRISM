// PRISM conformance task regress/uninit_elem_loop_true.c: expected true (memsafety)
// regression: local arrays: an element written only by some loop iterations is read uninitialised
int uninit_elem_loop_true(int n) {
    int a[4];
    if (n < 0) n = 0;
    for (int k = 0; k < 4; k++) a[k] = k + n % 8;
    return a[0];
}
