// PRISM conformance task array/arr_index_expr_true.c: expected true (no-oob)
int arr_index_expr_true(int i) {
    int a[10] = {0};
    if (i < 0 || i > 4) return 0;
    return a[2 * i + 1];
}
