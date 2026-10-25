// PRISM conformance task array/arr_2d_inner_true.c: expected true (no-oob)
// Each subscript is inside its own dimension.
int arr_2d_inner_true(int j) {
    int m[2][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
    if (j < 0 || j > 3) return 0;
    return m[0][j];
}
