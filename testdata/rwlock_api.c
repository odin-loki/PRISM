int pthread_rwlock_rdlock(void *rwlock);

void rwlock_bad(void) {
    pthread_rwlock_rdlock(0);
}

void rwlock_ok(void) {
    if (pthread_rwlock_rdlock(0) != 0)
        return;
}
