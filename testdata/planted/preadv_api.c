long preadv(int fd, const void *iov, int iovcnt, long offset);

void preadv_bad(void) {
    preadv(0, 0, 0, 0);
}

void preadv_ok(void) {
    if (preadv(0, 0, 0, 0) < 0)
        return;
}
