void pthread_once_unenc_bad(void) {
    pthread_once();
}

int pthread_once_unenc_ok(int n) {
    return n;
}
