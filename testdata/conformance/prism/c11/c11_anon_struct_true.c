// PRISM conformance task c11/c11_anon_struct_true.c: expected true (no-div0)
struct v { union { int i; unsigned u; }; struct { int lo, hi; }; };
int c11_anon_struct_true(int x) {
    struct v s = { .lo = 1, .hi = 2 };
    return x / s.hi;
}
