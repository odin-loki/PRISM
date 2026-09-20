int float64_unenc_bad(int n) {
    _Float64 x = n; (void)x;
    return n;
}

int float64_unenc_ok(int n) {
    return n;
}
