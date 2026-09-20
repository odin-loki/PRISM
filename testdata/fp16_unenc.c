int fp16_unenc_bad(int n) {
    __fp16 x = n; (void)x;
    return n;
}

int fp16_unenc_ok(int n) {
    return n;
}
