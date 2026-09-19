int memfd_secret(unsigned flags);
int close(int fd);

void mfds_bad(void) {
    int fd=memfd_secret(0);
    close(fd);
}

void mfds_ok(void) {
    int fd=memfd_secret(0);
    if (fd<0)
        return;
    close(fd);
}
