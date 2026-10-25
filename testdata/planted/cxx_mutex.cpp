int mutex_unenc_bad(int n) {
    std::mutex m;
    std::lock_guard<std::mutex> g(m);
    return n;
}

int mutex_unenc_ok(int n) {
    return n;
}
