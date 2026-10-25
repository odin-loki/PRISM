int bind(int fd, const void *addr, unsigned len);

void bind_bad(int fd) {
    bind(fd, 0, 0);
}

void bind_ok(int fd) {
    if (bind(fd, 0, 0) != 0)
        return;
}
