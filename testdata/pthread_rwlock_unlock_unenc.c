void pthread_rwlock_unlock_unenc_bad(void) {
    pthread_rwlock_unlock();
}

int pthread_rwlock_unlock_unenc_ok(int n) {
    return n;
}
