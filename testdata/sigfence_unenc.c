int sigfence_unenc_bad(int n) {
    atomic_signal_fence();
    return n;
}

int sigfence_unenc_ok(int n) {
    return n;
}
