// PRISM conformance task c23/c23_empty_init_false.c: expected false (no-oob)
int c23_empty_init_false(int i) {
    int a[6] = {};
    if (i < 0 || i > 6) return 0;
    return a[i];
}
