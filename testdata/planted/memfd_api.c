int memfd_create(const char *name, unsigned flags);
int close(int fd);

void memfd_bad(void) {
    int fd=memfd_create("x",0);
    close(fd);
}

void memfd_ok(void) {
    int fd=memfd_create("x",0);
    if (fd<0)
        return;
    close(fd);
}
