int inotify_rm_watch(int fd, int wd);

void inorm_bad(void) {
    inotify_rm_watch(0, 0);
}

void inorm_ok(void) {
    if (inotify_rm_watch(0, 0) != 0)
        return;
}
