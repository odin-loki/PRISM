// PRISM conformance task regress/uninit_elem_branch_true.c: expected true (memsafety)
// regression: local arrays: per-element initialisation is merged over both branches
int uninit_elem_branch_true(int c) {
    int a[2];
    if (c) { a[0] = 1; a[1] = 2; } else { a[0] = 3; a[1] = 4; }
    return a[1];
}
