void pthread_cond_unenc_bad(void) {
    pthread_cond_wait();
}

int pthread_cond_unenc_ok(int n) {
    return n;
}
