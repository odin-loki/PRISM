int tounqual_unenc_bad(int n) {
    __typeof_unqual__(n) y = n;
    return y;
}

int tounqual_unenc_ok(int n) {
    return n;
}
