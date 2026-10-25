int fcntl(int fd, int cmd, ...);

void fcntl_bad(int fd) {
    fcntl(fd, 0);
}

void fcntl_ok(int fd) {
    if (fcntl(fd, 0) < 0)
        return;
}
