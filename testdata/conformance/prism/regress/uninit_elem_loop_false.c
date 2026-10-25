// PRISM conformance task regress/uninit_elem_loop_false.c: expected false (memsafety)
// regression: local arrays: an element written only by some loop iterations is read uninitialised
int uninit_elem_loop_false(int n) {
    int a[4];
    for (int k = 0; k < n && k < 4; k++) a[k] = k;
    return a[0];
}
