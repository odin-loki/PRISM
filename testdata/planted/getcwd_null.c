char *getcwd(char *buf, unsigned long size);

void getcwd_bad(void) {
    char *p = getcwd(0, 0);
    p[0] = 0;
}

void getcwd_ok(void) {
    char *p = getcwd(0, 0);
    if (p)
        p[0] = 0;
}
