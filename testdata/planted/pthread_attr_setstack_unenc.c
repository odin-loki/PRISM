void pthread_attr_setstack_unenc_bad(void) {
    pthread_attr_setstack();
}

int pthread_attr_setstack_unenc_ok(int n) {
    return n;
}
