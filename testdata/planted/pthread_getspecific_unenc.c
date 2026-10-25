void pthread_getspecific_unenc_bad(void) {
    pthread_getspecific();
}

int pthread_getspecific_unenc_ok(int n) {
    return n;
}
