void pthread_attr_destroy_unenc_bad(void) {
    pthread_attr_destroy();
}

int pthread_attr_destroy_unenc_ok(int n) {
    return n;
}
