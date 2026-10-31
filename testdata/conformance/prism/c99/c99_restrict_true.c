// PRISM conformance task c99/c99_restrict_true.c: expected true (no-oob)
static void cp(int *restrict d, const int *restrict s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
int c99_restrict_true(int x) {
    int a[4] = {1, 2, 3, 4}, b[4];
    cp(b, a, 4);
    return b[3] + (x & 0xFF);
}
