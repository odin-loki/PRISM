int ulock_unenc_bad(int n) {
    std::unique_lock<int> g;
    return n;
}

int ulock_unenc_ok(int n) {
    return n;
}
