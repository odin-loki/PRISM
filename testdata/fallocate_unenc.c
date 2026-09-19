void fallocate_unenc_bad(void) {
    posix_fallocate();
}

int fallocate_unenc_ok(int n) {
    return n;
}
