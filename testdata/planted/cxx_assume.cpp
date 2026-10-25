int assume_unenc_bad(int n) {
    [[assume(n>0)]];
    return n;
}

int assume_unenc_ok(int n) {
    return n;
}
