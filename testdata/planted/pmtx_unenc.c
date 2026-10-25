int pmtx_unenc_bad(int n) {
    pthread_mutex_t m;
    (void)m;
    return n;
}

int pmtx_unenc_ok(int n) {
    return n;
}
