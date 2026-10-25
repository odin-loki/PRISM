int clz_unenc_bad(int n) {
    return __builtin_clz(n);
}

int clz_ok(int n) {
    return n;
}
