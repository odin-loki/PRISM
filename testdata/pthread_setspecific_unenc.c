void pthread_setspecific_unenc_bad(void) {
    pthread_setspecific();
}

int pthread_setspecific_unenc_ok(int n) {
    return n;
}
