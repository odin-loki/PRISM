// PRISM conformance task c23/c23_label_decl_true.c: expected true (no-div0)
int c23_label_decl_true(int x) {
    if (x) goto L;
    return 0;
L:
    int d = x;
    return 100 / d;
}
