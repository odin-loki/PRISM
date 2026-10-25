int smutex_unenc_bad(int n) {
    std::shared_mutex x;
    return n;
}

int smutex_unenc_ok(int n) {
    return n;
}
