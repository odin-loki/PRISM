char *strdup(const char *);

int strdup_bad(void) {
    char *s = strdup("x");
    return s[0];
}

int strdup_ok(void) {
    char *s = strdup("x");
    if (!s)
        return 0;
    return s[0];
}
