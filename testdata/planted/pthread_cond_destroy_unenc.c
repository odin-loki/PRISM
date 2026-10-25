void pthread_cond_destroy_unenc_bad(void) {
    pthread_cond_destroy();
}

int pthread_cond_destroy_unenc_ok(int n) {
    return n;
}
