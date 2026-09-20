int complex_unenc_bad(int n) {
    _Complex int z = n;
    (void)z;
    return n;
}

int complex_unenc_ok(int n) {
    return n;
}
