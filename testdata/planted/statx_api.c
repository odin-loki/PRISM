int statx(int dirfd, const char *path, int flags, unsigned mask, void *statxbuf);

void statx_bad(void) {
    statx(0,0,0,0,0);
}

void statx_ok(void) {
    if (statx(0,0,0,0,0)!=0)
        return;
}
