void sigpending_unenc_bad(void) {
    sigpending();
}

int sigpending_unenc_ok(int n) {
    return n;
}
