int fsync(int fd);

void fsync_bad(void) {
    fsync(0);
}

void fsync_ok(void) {
    if (fsync(0)!=0)
        return;
}
