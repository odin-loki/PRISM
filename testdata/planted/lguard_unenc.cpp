int lguard_unenc_bad(int n) {
    std::lock_guard x;
    return n;
}

int lguard_unenc_ok(int n) {
    return n;
}
