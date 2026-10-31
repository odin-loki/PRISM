// PRISM conformance task c99/c99_compound_literal_false.c: expected false (no-oob)
int c99_compound_literal_false(int x) {
    if (x < 0 || x > 4) return 0;
    int *p = (int[]){1, 2, 3, 4};
    return p[x];
}
