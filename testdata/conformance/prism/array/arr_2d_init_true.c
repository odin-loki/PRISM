// PRISM conformance task array/arr_2d_init_true.c: expected true (no-oob)
// Partial nested initialiser: the elements not listed are zero.
int arr_2d_init_true(int i, int j) {
    int m[3][3] = {{1}, {2, 3}};
    if (i < 0 || i > 2 || j < 0 || j > 2) return 0;
    return m[i][j] + 10 / (m[2][2] + 1);
}
