int msync(void);

void msync_bad(void) {
    msync();
}

void msync_ok(void) {
    if (msync()!=-1)
        return;
}
