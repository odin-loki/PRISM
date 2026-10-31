// PRISM conformance task c23/c23_typeof_unqual_true.c: expected true (no-div0)
int c23_typeof_unqual_true(int x) {
    const int c = (x & 7) | 1;
    typeof_unqual(c) d = c;
    d += 1;
    return 100 / d;
}
