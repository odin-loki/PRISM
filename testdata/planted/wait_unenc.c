void waitpid_unenc_bad(void) {
    waitpid();
}

int waitpid_unenc_ok(int n) {
    return n;
}

void wait_unenc_bad(void) {
    wait();
}

int wait_unenc_ok(int n) {
    return n;
}
