/* PIR tasks: a unit whose functions are all static and unused (csmith's
   --nomain output). clang emits no function for it, so the lowering falls
   back to -femit-all-decls; the signed-shift locator has to do the same, or
   the C signed left shift base check is dropped (a wrong PROVED). */
static int ss_shl_neg_bad(signed char p) { return p << 1; }
static int ss_shl_ok(signed char p) {
    if (p < 0)
        return 0;
    return p << 1;
}
static unsigned ss_shl_unsigned_ok(unsigned char p) { return (unsigned)p << 1; }
static int ss_fold_shift_bad(void) {
    int l = 3;
    (void)((short)1 >> (short)(l = 0xBEA1));
    return l;
}
