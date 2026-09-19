int utimensat(int dirfd, const char *pathname, const void *times, int flags);

void utimens_bad(void) {
    utimensat(0,0,0,0);
}

void utimens_ok(void) {
    if (utimensat(0,0,0,0)!=-1)
        return;
}
