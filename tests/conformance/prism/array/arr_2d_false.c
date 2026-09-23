// PRISM conformance task array/arr_2d_false.c: expected false (no-oob)
int arr_2d_false(int i, int j) {
    int m[3][4] = {{0}};
    if (i < 0 || i >= 4 || j < 0 || j >= 4) return 0;
    m[i][j] = 1;
    return m[0][0];
}
