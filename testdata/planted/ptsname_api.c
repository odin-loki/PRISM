char *ptsname(int fd);

void ptsname_bad(void) {
    char *s=ptsname(0);
    (void)s[0];
}

void ptsname_ok(void) {
    char *s=ptsname(0);
    if (!s)
        return;
    (void)s[0];
}
