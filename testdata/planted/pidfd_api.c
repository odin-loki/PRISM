int pidfd_open(int pid, unsigned flags);
int close(int fd);

void pidfd_bad(void) {
    int fd=pidfd_open(0,0);
    close(fd);
}

void pidfd_ok(void) {
    int fd=pidfd_open(0,0);
    if (fd<0)
        return;
    close(fd);
}
