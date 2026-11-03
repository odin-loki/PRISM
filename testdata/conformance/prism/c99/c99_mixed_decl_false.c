// PRISM conformance task c99/c99_mixed_decl_false.c: expected false (no-div0)
int c99_mixed_decl_false(int x) {
    int a = x & 3;
    int b = 12 / a;
    return b;
}
