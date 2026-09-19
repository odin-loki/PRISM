int stmutex_unenc_bad(int n) {
    std::shared_timed_mutex x;
    return n;
}

int stmutex_unenc_ok(int n) {
    return n;
}
