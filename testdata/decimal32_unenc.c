int decimal32_unenc_bad(int n) {
    _Decimal32 x = n; (void)x;
    return n;
}

int decimal32_unenc_ok(int n) {
    return n;
}
