int decimal_unenc_bad(int n) {
    _Decimal64 x = n;
    (void)x;
    return n;
}

int decimal_ok(int n) {
    return n;
}
