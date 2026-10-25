void pthread_attr_getstacksize_unenc_bad(void) {
    pthread_attr_getstacksize();
}

int pthread_attr_getstacksize_unenc_ok(int n) {
    return n;
}
