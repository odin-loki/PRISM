void munlock_unenc_bad(void) {
    munlock();
}

int munlock_unenc_ok(int n) {
    return n;
}
