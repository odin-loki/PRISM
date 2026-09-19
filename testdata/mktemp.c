char *mktemp(char *);
int mkstemp(char *);

void mktemp_bad(void) {
    char t[] = "/tmp/xXXXXXX";
    mktemp(t);
}

void mktemp_ok(void) {
    char t[] = "/tmp/xXXXXXX";
    mkstemp(t);
}
