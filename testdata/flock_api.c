int flock(int fd, int op);

void flock_bad(int fd) {
    flock(fd, 1);
}

void flock_ok(int fd) {
    if (flock(fd, 1) != 0)
        return;
}
