// PRISM conformance task array/arr_2d_init_false.c: expected false (no-oob)
// Partial nested initialiser; j == 3 is past the end of a row.
int arr_2d_init_false(int i, int j) {
    int m[3][3] = {{1}, {2, 3}};
    if (i < 0 || i > 2 || j < 0 || j > 3) return 0;
    return m[i][j] + 10 / (m[2][2] + 1);
}
