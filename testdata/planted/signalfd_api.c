int signalfd(int fd, const void *mask, int flags);
int close(int fd);

void signalfd_bad(void) {
    int fd=signalfd(0,0,0);
    close(fd);
}

void signalfd_ok(void) {
    int fd=signalfd(0,0,0);
    if (fd<0)
        return;
    close(fd);
}
