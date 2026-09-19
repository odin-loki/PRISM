char *realpath(const char *path, char *resolved);

void realpath_bad(void) {
    char *p = realpath(".", 0);
    p[0] = 0;
}

void realpath_ok(void) {
    char *p = realpath(".", 0);
    if (p)
        p[0] = 0;
}
