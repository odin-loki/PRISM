// PRISM conformance task array/arr_sum_false.c: expected false (no-oob)
int arr_sum_false(int n) {
    int a[5] = {1, 2, 3, 4, 5};
    int s = 0;
    if (n > 6) n = 6;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}
