void pthread_rwlock_trywrlock_unenc_bad(void) {
    pthread_rwlock_trywrlock();
}

int pthread_rwlock_trywrlock_unenc_ok(int n) {
    return n;
}
