/* Out of bounds write. */
int oob_write(int i) {
    int a[4];
    a[i] = 1;
    return a[0];
}
