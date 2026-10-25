int faccessat2(int dirfd, const char *pathname, int mode, int flags);

void faccessat2_bad(void) {
    faccessat2(0, "x", 0, 0);
}

void faccessat2_ok(void) {
    if (faccessat2(0, "x", 0, 0) != 0)
        return;
}
