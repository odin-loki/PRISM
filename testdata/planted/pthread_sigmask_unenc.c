void pthread_sigmask_unenc_bad(void) {
    pthread_sigmask();
}

int pthread_sigmask_unenc_ok(int n) {
    return n;
}
