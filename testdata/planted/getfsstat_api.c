int getfsstat(void *buf, long bufsize, int mode);

void getfsstat_bad(void) {
    getfsstat(0, 0, 0);
}

void getfsstat_ok(void) {
    if (getfsstat(0, 0, 0) < 0)
        return;
}
