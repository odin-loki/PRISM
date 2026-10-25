int fstatat(int dirfd, const char *pathname, void *statbuf, int flags);

void fstatat_bad(void) {
    fstatat(0, "x", 0, 0);
}

void fstatat_ok(void) {
    if (fstatat(0, "x", 0, 0) != 0)
        return;
}
