// PRISM conformance task c23/c23_typeof_unqual_false.c: expected false (no-div0)
int c23_typeof_unqual_false(int x) {
    const int c = x & 7;
    typeof_unqual(c) d = c;
    return 100 / d;
}
