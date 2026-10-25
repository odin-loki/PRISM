int clz_zero_bad(void) {
    return __builtin_clz(0);
}

int clz_zero_ok(unsigned n) {
    if (!n)
        return 0;
    return __builtin_clz(n);
}
