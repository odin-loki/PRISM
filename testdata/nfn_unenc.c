int nfn_unenc_bad(int n) {
    void inner(void) { }
    return n;
}

int nfn_unenc_ok(int n) {
    return n;
}
