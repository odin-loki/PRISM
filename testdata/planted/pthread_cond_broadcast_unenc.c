void pthread_cond_broadcast_unenc_bad(void) {
    pthread_cond_broadcast();
}

int pthread_cond_broadcast_unenc_ok(int n) {
    return n;
}
