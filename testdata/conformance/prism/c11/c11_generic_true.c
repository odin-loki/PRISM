// PRISM conformance task c11/c11_generic_true.c: expected true (no-div0)
#define DEN(v) _Generic((v), int: (v) | 1, default: 1)
int c11_generic_true(int x) {
    return 100 / DEN(x);
}
