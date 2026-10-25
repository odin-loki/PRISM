void mmap_unenc_bad(void) {
    mmap(0, 0, 0, 0, 0, 0);
}

int mmap_unenc_ok(int n) {
    return n;
}
