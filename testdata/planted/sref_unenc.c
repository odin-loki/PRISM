int sref_unenc_bad(int n) {
    std::ref();
    return n;
}

int sref_unenc_ok(int n) {
    return n;
}
