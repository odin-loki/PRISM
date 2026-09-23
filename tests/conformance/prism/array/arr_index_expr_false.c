// PRISM conformance task array/arr_index_expr_false.c: expected false (no-oob)
int arr_index_expr_false(int i) {
    int a[10] = {0};
    if (i < 0 || i > 4) return 0;
    return a[2 * i + 2];
}
