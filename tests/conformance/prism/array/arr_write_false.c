// PRISM conformance task array/arr_write_false.c: expected false (no-oob)
int arr_write_false(int i, int v) {
    int a[4] = {0};
    if (i > 3) return 0;
    a[i] = v;
    return a[0];
}
