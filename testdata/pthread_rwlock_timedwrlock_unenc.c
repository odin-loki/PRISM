void pthread_rwlock_timedwrlock_unenc_bad(void) {
    pthread_rwlock_timedwrlock();
}

int pthread_rwlock_timedwrlock_unenc_ok(int n) {
    return n;
}
