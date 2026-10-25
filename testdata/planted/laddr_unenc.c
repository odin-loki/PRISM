int laddr_unenc_bad(int n) {
    void *p = &&lab;
    (void)p;
lab:
    return n;
}

int laddr_unenc_ok(int n) {
    return n;
}
