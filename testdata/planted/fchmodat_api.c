int fchmodat(int dirfd, const char *pathname, unsigned mode, int flags);

void fchmodat_bad(void) {
    fchmodat(0, "x", 0, 0);
}

void fchmodat_ok(void) {
    if (fchmodat(0, "x", 0, 0) != 0)
        return;
}
