int float16_unenc_bad(int n) {
    _Float16 x = n;
    (void)x;
    return n;
}

int float16_ok(int n) {
    return n;
}
