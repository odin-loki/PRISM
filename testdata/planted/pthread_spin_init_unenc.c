void pthread_spin_init_unenc_bad(void) {
    pthread_spin_init();
}

int pthread_spin_init_unenc_ok(int n) {
    return n;
}
