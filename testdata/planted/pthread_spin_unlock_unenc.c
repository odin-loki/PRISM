void pthread_spin_unlock_unenc_bad(void) {
    pthread_spin_unlock();
}

int pthread_spin_unlock_unenc_ok(int n) {
    return n;
}
