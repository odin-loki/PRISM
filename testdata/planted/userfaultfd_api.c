int userfaultfd(int flags);
int close(int fd);

void uffd_bad(void) {
    int fd=userfaultfd(0);
    close(fd);
}

void uffd_ok(void) {
    int fd=userfaultfd(0);
    if (fd<0)
        return;
    close(fd);
}
