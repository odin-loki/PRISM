int rtmutex_unenc_bad(int n) {
    std::recursive_timed_mutex x;
    return n;
}

int rtmutex_unenc_ok(int n) {
    return n;
}
