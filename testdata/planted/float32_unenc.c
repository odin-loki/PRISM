int float32_unenc_bad(int n) {
    _Float32 x = n; (void)x;
    return n;
}

int float32_unenc_ok(int n) {
    return n;
}
