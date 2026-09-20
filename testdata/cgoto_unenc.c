int cgoto_unenc_bad(int n) {
    void *p = &&lab;
    goto *p;
lab:
    return n;
}

int cgoto_unenc_ok(int n) {
    return n;
}
