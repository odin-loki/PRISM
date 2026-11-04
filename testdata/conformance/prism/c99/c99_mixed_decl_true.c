// PRISM conformance task c99/c99_mixed_decl_true.c: expected true (no-div0)
int c99_mixed_decl_true(int x) {
    int a = x & 3;
    a += 1;
    int b = 12 / a;
    return b;
}
