int fsopen(const char *fsname, unsigned flags);
int close(int fd);

void fsopen_bad(void) {
    int fd=fsopen("x",0);
    close(fd);
}

void fsopen_ok(void) {
    int fd=fsopen("x",0);
    if (fd<0)
        return;
    close(fd);
}
