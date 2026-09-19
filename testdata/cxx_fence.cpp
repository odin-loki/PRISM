int fence_unenc_bad(int n) {
    std::atomic_thread_fence();
    return n;
}

int fence_unenc_ok(int n) {
    return n;
}
