void pthread_spin_trylock_unenc_bad(void) {
    pthread_spin_trylock();
}

int pthread_spin_trylock_unenc_ok(int n) {
    return n;
}
