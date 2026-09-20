void futex_wake_unenc_bad(void) {
    futex_wake();
}

int futex_wake_unenc_ok(int n) {
    return n;
}
