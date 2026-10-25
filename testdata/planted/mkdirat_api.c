int mkdirat(int dirfd, const char *pathname, unsigned mode);

void mkdirat_bad(void) {
    mkdirat(0, "x", 0755);
}

void mkdirat_ok(void) {
    if (mkdirat(0, "x", 0755) != 0)
        return;
}
