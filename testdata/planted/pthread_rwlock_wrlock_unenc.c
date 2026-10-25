void pthread_rwlock_wrlock_unenc_bad(void) {
    pthread_rwlock_wrlock();
}

int pthread_rwlock_wrlock_unenc_ok(int n) {
    return n;
}
