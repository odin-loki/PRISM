int choose_unenc_bad(int n) {
    return __builtin_choose_expr(1, n, 0);
}

int choose_ok(int n) {
    return n;
}
