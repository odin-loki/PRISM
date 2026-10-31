// PRISM conformance task c99/c99_inline_false.c: expected false (no-overflow)
static inline int twice(int v) { return v * 2; }
int c99_inline_false(int x) {
    return twice(x);
}
