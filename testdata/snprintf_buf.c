int snprintf(char *s, unsigned n, const char *fmt, ...);

void snprintf_bad(void) {
    char b[4];
    snprintf(b, 64, "%s", "x");
}

void snprintf_ok(void) {
    char b[4];
    snprintf(b, sizeof b, "x");
}
