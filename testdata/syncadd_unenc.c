void syncadd_unenc_bad(void) {
    __sync_fetch_and_add();
}

int syncadd_unenc_ok(int n) {
    return n;
}
