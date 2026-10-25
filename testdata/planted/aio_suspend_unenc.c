void aio_suspend_unenc_bad(void) {
    aio_suspend();
}

int aio_suspend_unenc_ok(int n) {
    return n;
}
