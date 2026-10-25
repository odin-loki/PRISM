// PRISM conformance task shift/char_shift_true.c: expected true (no-shift-ub)
// unsigned char promotes to int; 255<<20 fits
int char_shift_true(unsigned char c) {
    return c << 20;
}
