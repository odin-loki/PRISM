void pthread_cond_timedwait_unenc_bad(void) {
    pthread_cond_timedwait();
}

int pthread_cond_timedwait_unenc_ok(int n) {
    return n;
}
