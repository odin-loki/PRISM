int faccessat(int dirfd, const char *pathname, int mode, int flags);

void faccessat_bad(void) {
    faccessat(0, "x", 0, 0);
}

void faccessat_ok(void) {
    if (faccessat(0, "x", 0, 0) != 0)
        return;
}
