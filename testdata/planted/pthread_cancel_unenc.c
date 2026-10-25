void pthread_cancel_unenc_bad(void) {
    pthread_cancel();
}

int pthread_cancel_unenc_ok(int n) {
    return n;
}
