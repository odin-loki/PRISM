// PRISM conformance task c23/c23_label_decl_false.c: expected false (no-div0)
int c23_label_decl_false(int x) {
    goto L;
L:
    int d = x;
    return 100 / d;
}
