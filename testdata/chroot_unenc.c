void chroot_unenc_bad(void) {
    chroot("/j");
}

int chroot_unenc_ok(int n) {
    return n;
}
