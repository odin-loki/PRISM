int cpost_unenc_bad(int n) {
    [[post: n]];
    return n;
}

int cpost_unenc_ok(int n) {
    return n;
}
