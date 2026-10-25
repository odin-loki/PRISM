void pthread_barrier_unenc_bad(void) {
    pthread_barrier_wait();
}

int pthread_barrier_unenc_ok(int n) {
    return n;
}
