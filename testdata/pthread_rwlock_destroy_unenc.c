void pthread_rwlock_destroy_unenc_bad(void) {
    pthread_rwlock_destroy();
}

int pthread_rwlock_destroy_unenc_ok(int n) {
    return n;
}
