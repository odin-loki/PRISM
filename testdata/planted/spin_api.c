int pthread_spin_lock(void *lock);

void spin_bad(void) {
    pthread_spin_lock(0);
}

void spin_ok(void) {
    if (pthread_spin_lock(0) != 0)
        return;
}
