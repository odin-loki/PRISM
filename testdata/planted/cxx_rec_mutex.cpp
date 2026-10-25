int rmutex_unenc_bad(int n) {
    std::recursive_mutex x;
    return n;
}

int rmutex_unenc_ok(int n) {
    return n;
}
