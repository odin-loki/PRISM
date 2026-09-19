void rwlock_unenc_bad(void) {
    pthread_rwlock_rdlock();
}

int rwlock_unenc_ok(int n) {
    return n;
}
