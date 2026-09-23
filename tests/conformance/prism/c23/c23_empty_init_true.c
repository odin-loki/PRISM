// PRISM conformance task c23/c23_empty_init_true.c: expected true (no-oob)
// C23 empty initializer
int c23_empty_init_true(int i) {
    int a[6] = {};
    if (i < 0 || i > 5) return 0;
    return a[i];
}
