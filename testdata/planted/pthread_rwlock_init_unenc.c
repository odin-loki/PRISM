void pthread_rwlock_init_unenc_bad(void) {
    pthread_rwlock_init();
}

int pthread_rwlock_init_unenc_ok(int n) {
    return n;
}
