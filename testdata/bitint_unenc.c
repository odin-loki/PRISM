int bitint_unenc_bad(int n) {
    _BitInt x = n; (void)x;
    return n;
}

int bitint_unenc_ok(int n) {
    return n;
}
