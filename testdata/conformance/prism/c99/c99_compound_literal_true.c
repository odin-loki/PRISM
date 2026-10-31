// PRISM conformance task c99/c99_compound_literal_true.c: expected true (no-oob)
int c99_compound_literal_true(int x) {
    if (x < 0 || x > 3) return 0;
    int *p = (int[]){1, 2, 3, 4};
    return p[x];
}
