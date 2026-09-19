int fanotify_init(unsigned flags, unsigned event_f_flags);
int close(int fd);

void fanotify_bad(void) {
    int fd=fanotify_init(0,0);
    close(fd);
}

void fanotify_ok(void) {
    int fd=fanotify_init(0,0);
    if (fd<0)
        return;
    close(fd);
}
