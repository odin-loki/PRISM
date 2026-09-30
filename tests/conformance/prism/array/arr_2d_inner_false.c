// PRISM conformance task array/arr_2d_inner_false.c: expected false (no-oob)
// m[0][j] with j == 4 stays inside the 8-element object but not inside row 0.
int arr_2d_inner_false(int j) {
    int m[2][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
    if (j < 0 || j > 4) return 0;
    return m[0][j];
}
