int inotify_init(void);
int close(int fd);

void inotify_bad(void) {
    int fd=inotify_init();
    close(fd);
}

void inotify_ok(void) {
    int fd=inotify_init();
    if (fd<0)
        return;
    close(fd);
}
