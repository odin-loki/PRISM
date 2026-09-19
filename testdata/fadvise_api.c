int posix_fadvise(int fd, long offset, long len, int advice);

void fadvise_bad(void) {
    posix_fadvise(0, 0, 0, 0);
}

void fadvise_ok(void) {
    if (posix_fadvise(0, 0, 0, 0) != 0)
        return;
}
