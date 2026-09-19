int unlinkat(int dirfd, const char *pathname, int flags);

void unlinkat_bad(void) {
    unlinkat(0, "x", 0);
}

void unlinkat_ok(void) {
    if (unlinkat(0, "x", 0) != 0)
        return;
}
