int openat(int dirfd, const char *path, int flags);
int write(int fd, const void *buf, unsigned n);

void openat_bad(void) {
    int fd = openat(0, "x", 0);
    write(fd, 0, 0);
}

void openat_ok(void) {
    int fd;
    if ((fd = openat(0, "x", 0)) < 0)
        return;
}
