// PRISM conformance task c99/c99_static_param_true.c: expected true (no-oob)
static int sum4(const int a[static 4]) { return a[0] + a[3]; }
int c99_static_param_true(short x) {
    int b[4] = {x, 0, 0, 1};
    return sum4(b);
}
