// PRISM conformance task c11/c11_generic_false.c: expected false (no-div0)
#define DEN(v) _Generic((v), int: (v) | 1, default: 1)
int c11_generic_false(int x) {
    return 100 / _Generic(x, int: x, default: 1);
}
