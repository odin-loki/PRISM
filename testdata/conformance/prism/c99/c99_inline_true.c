// PRISM conformance task c99/c99_inline_true.c: expected true (no-overflow)
static inline int twice(int v) { return v * 2; }
int c99_inline_true(int x) {
    return twice(x & 0xFFFF);
}
