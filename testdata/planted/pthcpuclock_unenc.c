void pthcpuclock_unenc_bad(void) {
    pthread_getcpuclockid();
}

int pthcpuclock_unenc_ok(int n) {
    return n;
}
