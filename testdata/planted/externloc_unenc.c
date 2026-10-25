int externloc_unenc_bad(int n) {
    extern int x;
    x = n;
    return x;
}

int externloc_unenc_ok(int n) {
    return n;
}
