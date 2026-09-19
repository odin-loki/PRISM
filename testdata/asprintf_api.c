int asprintf(char **strp, const char *fmt, ...);
int vasprintf(char **strp, const char *fmt, void *ap);

void asprintf_bad(void) {
    asprintf(0,0);
}

void asprintf_ok(void) {
    if (asprintf(0,0)<0)
        return;
}
