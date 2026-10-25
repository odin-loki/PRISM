// PRISM conformance task array/arr_write_true.c: expected true (no-oob)
int arr_write_true(int i, int v) {
    int a[4] = {0};
    if (i < 0 || i > 3) return 0;
    a[i] = v;
    return a[0];
}
