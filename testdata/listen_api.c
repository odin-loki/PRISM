int listen(int fd, int backlog);

void listen_bad(int fd) {
    listen(fd, 1);
}

void listen_ok(int fd) {
    if (listen(fd, 1) != 0)
        return;
}
