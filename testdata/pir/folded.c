/* PIR tasks: undefined behaviour that clang folds at -O0. */
int fold_shift_bad(void) { return 1 << 31; }
int fold_div_bad(int x) {
    if (x > 5)
        return 7 / 0;
    return x;
}
int fold_ok(void) { return 1 << 30; }
/* The shift count (or base) is an assignment whose value clang folds to a
   constant, so the shift is folded away with neither an IR instruction nor a
   Sema diagnostic (csmith: `(int16_t)1 >> (int16_t)(l = 0xBEA1L)`). */
int fold_shift_assign_bad(void) {
    int l = 3;
    (void)((short)1 >> (short)(l = 0xBEA1));
    return l;
}
int fold_shl_neg_assign_bad(void) {
    int l = 0;
    (void)((-1) << (l = 1));
    return l;
}
int fold_shift_assign_ok(void) {
    int l = 3;
    (void)((short)1 >> (short)(l = 2));
    return l;
}
