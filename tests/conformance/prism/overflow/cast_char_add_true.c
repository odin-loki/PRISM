// PRISM conformance task overflow/cast_char_add_true.c: expected true (no-overflow)
// narrowing to signed char is implementation-defined, not UB
int cast_char_add_true(int a) {
    signed char c = (signed char)a;
    return c + 1000;
}
