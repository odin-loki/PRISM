// PRISM conformance task array/arr_negative_true.c: expected true (no-oob)
int arr_negative_true(int i) {
    int a[4] = {9, 8, 7, 6};
    if (i < 1 || i > 4) return 0;
    return a[i - 1];
}
