int aenum_unenc_bad(int n) {
    enum { RED = 1 } e;
    e = RED;
    return e + n;
}

int aenum_unenc_ok(int n) {
    return n;
}
