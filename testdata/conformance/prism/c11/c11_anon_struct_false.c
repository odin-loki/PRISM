// PRISM conformance task c11/c11_anon_struct_false.c: expected false (no-div0)
struct v { union { int i; unsigned u; }; struct { int lo, hi; }; };
int c11_anon_struct_false(int x) {
    struct v s = { .lo = 1 };
    return x / s.hi;
}
