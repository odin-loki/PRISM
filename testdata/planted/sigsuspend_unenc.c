void sigsuspend_unenc_bad(void) {
    sigsuspend();
}

int sigsuspend_unenc_ok(int n) {
    return n;
}
