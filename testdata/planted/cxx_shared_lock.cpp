int slock_unenc_bad(int n) {
    std::shared_lock<int> x;
    return n;
}

int slock_unenc_ok(int n) {
    return n;
}
