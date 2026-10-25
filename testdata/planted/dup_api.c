int dup(int fd);
int write(int fd, const void *buf, unsigned n);

void dup_bad(int fd) {
    int n = dup(fd);
    write(n, 0, 0);
}

void dup_ok(int fd) {
    int n;
    if ((n = dup(fd)) < 0)
        return;
}
