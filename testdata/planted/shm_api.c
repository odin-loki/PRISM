int shm_open(const char *name, int oflag, unsigned mode);
int close(int fd);

void shm_bad(void) {
    int fd = shm_open("x", 0, 0);
    close(fd);
}

void shm_ok(void) {
    int fd = shm_open("x", 0, 0);
    if (fd < 0)
        return;
    close(fd);
}
