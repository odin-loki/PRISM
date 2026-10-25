// PRISM conformance task macro/fn_macro_true.c: expected true (no-overflow)
#define SQ(x) ((x) * (x))
int fn_macro_true(int a) {
    if (a < -46340 || a > 46340) return 0;
    return SQ(a);
}
