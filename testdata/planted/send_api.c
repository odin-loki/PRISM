int send(int fd, const void *buf, unsigned n, int flags);

void send_bad(int fd) {
    send(fd, 0, 0, 0);
}

void send_ok(int fd) {
    if (send(fd, 0, 0, 0) < 0)
        return;
}
