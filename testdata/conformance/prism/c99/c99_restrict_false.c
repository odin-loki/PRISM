// PRISM conformance task c99/c99_restrict_false.c: expected false (no-oob)
static void cp(int *restrict d, const int *restrict s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
int c99_restrict_false(int x) {
    int a[4] = {1, 2, 3, 4}, b[4];
    if (x < 0 || x > 5) return 0;
    cp(b, a, x);
    return b[0];
}
