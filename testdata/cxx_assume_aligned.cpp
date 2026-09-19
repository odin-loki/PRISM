int asmalign_unenc_bad(int n) {
    std::assume_aligned();
    return n;
}

int asmalign_unenc_ok(int n) {
    return n;
}
