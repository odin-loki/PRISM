void munlockall_unenc_bad(void) {
    munlockall();
}

int munlockall_unenc_ok(int n) {
    return n;
}
