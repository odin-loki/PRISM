int openat2(int dirfd, const char *path, void *how, unsigned size);
int close(int fd);

void openat2_bad(void) {
    int fd=openat2(0,"x",0,0);
    close(fd);
}

void openat2_ok(void) {
    int fd=openat2(0,"x",0,0);
    if (fd<0)
        return;
    close(fd);
}
