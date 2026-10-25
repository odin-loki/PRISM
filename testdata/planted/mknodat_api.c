int mknodat(int dirfd, const char *pathname, unsigned mode, unsigned dev);

void mknodat_bad(void) {
    mknodat(0, "x", 0, 0);
}

void mknodat_ok(void) {
    if (mknodat(0, "x", 0, 0) != 0)
        return;
}
