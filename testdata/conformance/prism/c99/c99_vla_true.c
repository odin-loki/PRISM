// PRISM conformance task c99/c99_vla_true.c: expected true (no-oob)
int c99_vla_true(int x) {
    if (x < 1 || x > 16) return 0;
    int a[x];
    for (int i = 0; i < x; i++) a[i] = i;
    return a[x - 1];
}
