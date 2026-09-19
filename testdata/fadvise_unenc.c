void fadvise_unenc_bad(void) {
    posix_fadvise();
}

int fadvise_unenc_ok(int n) {
    return n;
}
