int connect(int fd, const void *addr, unsigned len);

void connect_bad(int fd) {
    connect(fd, 0, 0);
}

void connect_ok(int fd) {
    if (connect(fd, 0, 0) != 0)
        return;
}
