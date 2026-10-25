int wordexp(const char *s, void *p, int flags);

void wordexp_bad(void) {
    wordexp("*",0,0);
}

void wordexp_ok(void) {
    if (wordexp("*",0,0)!=0)
        return;
}
