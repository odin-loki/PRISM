void sigaltstack_unenc_bad(void) {
    sigaltstack();
}

int sigaltstack_unenc_ok(int n) {
    return n;
}
