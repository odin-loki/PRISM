void pthread_key_unenc_bad(void) {
    pthread_key_create();
}

int pthread_key_unenc_ok(int n) {
    return n;
}
