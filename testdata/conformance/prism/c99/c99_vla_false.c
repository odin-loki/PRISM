// PRISM conformance task c99/c99_vla_false.c: expected false (no-oob)
int c99_vla_false(int x) {
    if (x < 1 || x > 16) return 0;
    int a[x];
    a[x] = 1;
    return a[0];
}
