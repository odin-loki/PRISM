// PRISM conformance task macro/fn_macro_false.c: expected false (no-overflow)
#define SQ(x) ((x) * (x))
int fn_macro_false(int a) {
    return SQ(a);
}
