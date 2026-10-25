// PRISM conformance task array/arr_2d_true.c: expected true (no-oob)
int arr_2d_true(int i, int j) {
    int m[3][4] = {{0}};
    if (i < 0 || i >= 3 || j < 0 || j >= 4) return 0;
    m[i][j] = 1;
    return m[i][j];
}
