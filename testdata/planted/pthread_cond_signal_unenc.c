void pthread_cond_signal_unenc_bad(void) {
    pthread_cond_signal();
}

int pthread_cond_signal_unenc_ok(int n) {
    return n;
}
