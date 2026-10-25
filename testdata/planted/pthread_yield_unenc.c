void pthread_yield_unenc_bad(void) {
    pthread_yield();
}

int pthread_yield_unenc_ok(int n) {
    return n;
}
