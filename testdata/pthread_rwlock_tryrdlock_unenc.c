void pthread_rwlock_tryrdlock_unenc_bad(void) {
    pthread_rwlock_tryrdlock();
}

int pthread_rwlock_tryrdlock_unenc_ok(int n) {
    return n;
}
