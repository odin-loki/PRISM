// PRISM conformance task overflow/char_promote_false.c: expected false (no-overflow)
int char_promote_false(signed char a, int b) {
    if (b < 0) return 0;
    return a * b * 16909320;
}
