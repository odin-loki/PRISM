int staticloc_unenc_bad(int n) {
    static int x;
    x = n;
    return x;
}

int staticloc_unenc_ok(int n) {
    return n;
}
