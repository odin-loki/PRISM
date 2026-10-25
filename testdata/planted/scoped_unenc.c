int scoped_unenc_bad(int n) {
    std::scoped_lock x;
    return n;
}

int scoped_unenc_ok(int n) {
    return n;
}
