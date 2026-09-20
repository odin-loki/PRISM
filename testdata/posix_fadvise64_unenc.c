void posix_fadvise64_unenc_bad(void) {
    posix_fadvise64();
}

int posix_fadvise64_unenc_ok(int n) {
    return n;
}
