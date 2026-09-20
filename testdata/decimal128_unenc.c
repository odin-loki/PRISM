int decimal128_unenc_bad(int n) {
    _Decimal128 x = n; (void)x;
    return n;
}

int decimal128_unenc_ok(int n) {
    return n;
}
