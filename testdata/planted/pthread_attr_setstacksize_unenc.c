void pthread_attr_setstacksize_unenc_bad(void) {
    pthread_attr_setstacksize();
}

int pthread_attr_setstacksize_unenc_ok(int n) {
    return n;
}
