void pthread_spin_destroy_unenc_bad(void) {
    pthread_spin_destroy();
}

int pthread_spin_destroy_unenc_ok(int n) {
    return n;
}
