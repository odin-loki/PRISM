int mkfifo(const char *path, unsigned mode);
int mknod(const char *path, unsigned mode, unsigned dev);

void mkfifo_bad(void) {
    mkfifo("x", 0666);
}

void mkfifo_ok(void) {
    if (mkfifo("x", 0666) != 0)
        return;
}
