// PRISM conformance task array/arr_negative_false.c: expected false (no-oob)
int arr_negative_false(int i) {
    int a[4] = {9, 8, 7, 6};
    if (i > 4) return 0;
    return a[i - 1];
}
