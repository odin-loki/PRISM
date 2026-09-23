// PRISM conformance task array/arr_read_true.c: expected true (no-oob)
int arr_read_true(int i) {
    int a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    if (i < 0 || i >= 8) return 0;
    return a[i];
}
