char *getenv(const char *);

int getenv_bad(void) {
    char *s = getenv("X");
    return s[0];
}

int getenv_ok(void) {
    char *s = getenv("X");
    if (!s)
        return 0;
    return s[0];
}
