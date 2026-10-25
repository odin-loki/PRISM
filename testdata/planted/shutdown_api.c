int shutdown(int fd, int how);

void shutdown_bad(int fd) {
    shutdown(fd, 0);
}

void shutdown_ok(int fd) {
    if (shutdown(fd, 0) != 0)
        return;
}
