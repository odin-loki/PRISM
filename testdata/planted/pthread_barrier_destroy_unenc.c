void pthread_barrier_destroy_unenc_bad(void) {
    pthread_barrier_destroy();
}

int pthread_barrier_destroy_unenc_ok(int n) {
    return n;
}
