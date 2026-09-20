int twnested_unenc_bad(int n) {
    throw_with_nested();
    return n;
}

int twnested_unenc_ok(int n) {
    return n;
}
