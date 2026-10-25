void astore_unenc_bad(void) {
    __atomic_store();
}

int astore_unenc_ok(int n) {
    return n;
}
