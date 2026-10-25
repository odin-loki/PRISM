char *tmpnam(char *);
char *tempnam(const char *, const char *);
int mkstemp(char *);

void tmpnam_bad(void) {
    char buf[256];
    tmpnam(buf);
}

void tempnam_bad(void) {
    char *p = tempnam("/tmp", "pfx");
    (void)p;
}

void tmpnam_ok(void) {
    char t[] = "/tmp/xXXXXXX";
    mkstemp(t);
}

/* comment mentions tmpnam() but is not a call */
