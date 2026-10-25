// PRISM conformance task shift/char_shift_false.c: expected false (no-shift-ub)
// unsigned char promotes to int; 255<<24 does not fit in int
int char_shift_false(unsigned char c) {
    return c << 24;
}
