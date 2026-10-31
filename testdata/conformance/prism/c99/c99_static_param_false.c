// PRISM conformance task c99/c99_static_param_false.c: expected false (no-oob)
static int at(const int a[static 4], int i) { return a[i]; }
int c99_static_param_false(int i) {
    int b[4] = {1, 2, 3, 4};
    return at(b, i & 7);
}
