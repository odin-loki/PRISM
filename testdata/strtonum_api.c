long strtonum(const char *nptr, long minval, long maxval, const char **errstr);

void strtonum_bad(void) {
    strtonum("1", 0, 10, 0);
}

void strtonum_ok(void) {
    if (strtonum("1", 0, 10, 0) == 0)
        return;
}
