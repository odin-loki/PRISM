void pthread_rwlock_timedrdlock_unenc_bad(void) {
    pthread_rwlock_timedrdlock();
}

int pthread_rwlock_timedrdlock_unenc_ok(int n) {
    return n;
}
