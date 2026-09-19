void spin_unenc_bad(void) {
    pthread_spin_lock();
}

int spin_unenc_ok(int n) {
    return n;
}
