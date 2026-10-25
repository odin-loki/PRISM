void ctz_unenc_bad(void) {
    __builtin_ctz();
}

int ctz_unenc_ok(int n) {
    return n;
}
