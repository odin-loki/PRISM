void umask_unenc_bad(void) {
    umask(0);
}

int umask_unenc_ok(int n) {
    return n;
}
