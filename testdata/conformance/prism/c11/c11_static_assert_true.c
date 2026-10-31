// PRISM conformance task c11/c11_static_assert_true.c: expected true (no-oob)
_Static_assert(sizeof(int) == 4, "int is 32 bits");
int c11_static_assert_true(int x) {
    int a[sizeof(int)];
    if (x < 0 || x >= 4) return 0;
    a[x] = 1;
    return a[x];
}
