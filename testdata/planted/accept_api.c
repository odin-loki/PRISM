int accept(int s, void *addr, unsigned *len);
int accept4(int s, void *addr, unsigned *len, int flags);
int close(int fd);

int accept_bad(int s) {
    int fd = accept(s, 0, 0);
    return close(fd);
}

int accept_ok(int s) {
    int fd = accept(s, 0, 0);
    if (fd < 0)
        return -1;
    return close(fd);
}
