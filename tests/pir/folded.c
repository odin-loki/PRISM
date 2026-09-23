/* PIR tasks: undefined behaviour that clang folds at -O0. */
int fold_shift_bad(void) { return 1 << 31; }
int fold_div_bad(int x) {
    if (x > 5)
        return 7 / 0;
    return x;
}
int fold_ok(void) { return 1 << 30; }
