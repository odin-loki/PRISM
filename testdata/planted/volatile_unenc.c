int volatile_unenc_bad(int n) {
    volatile int y;
    y = n;
    return y;
}

int volatile_unenc_ok(int n) {
    return n;
}
