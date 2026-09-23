// PRISM conformance task regress/uninit_elem_branch_false.c: expected false (memsafety)
// regression: local arrays: per-element initialisation is merged over both branches
int uninit_elem_branch_false(int c) {
    int a[2];
    if (c) a[1] = 2;
    a[0] = 1;
    return a[1];
}
