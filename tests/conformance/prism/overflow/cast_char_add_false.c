// PRISM conformance task overflow/cast_char_add_false.c: expected false (no-overflow)
int cast_char_add_false(int a) {
    signed char c = (signed char)a;
    return c * 16909321 * 2;
}
