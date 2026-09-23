// PRISM conformance task array/arr_sum_true.c: expected true (no-oob)
int arr_sum_true(int n) {
    int a[5] = {1, 2, 3, 4, 5};
    int s = 0;
    if (n > 5) n = 5;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}
