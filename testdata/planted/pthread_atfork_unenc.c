void pthread_atfork_unenc_bad(void) {
    pthread_atfork();
}

int pthread_atfork_unenc_ok(int n) {
    return n;
}
