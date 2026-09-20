void pthread_barrier_init_unenc_bad(void) {
    pthread_barrier_init();
}

int pthread_barrier_init_unenc_ok(int n) {
    return n;
}
