void fdatasync_unenc_bad(void) {
    fdatasync();
}

int fdatasync_unenc_ok(int n) {
    return n;
}
