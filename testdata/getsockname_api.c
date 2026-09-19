int getsockname(int fd, void *addr, unsigned *len);

void getsock_bad(void) {
    getsockname(0, 0, 0);
}

void getsock_ok(void) {
    if (getsockname(0, 0, 0) != 0)
        return;
}
