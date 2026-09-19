int mkstemp(char *t);
int mkstemps(char *t, int slen);
char *mkdtemp(char *t);

void mkstemp_bad(void) {
    mkstemp("/tmp/xXXXXXX");
}

void mkstemps_bad(void) {
    mkstemps("/tmp/xXXXXXX", 0);
}

void mkstemp_ok(void) {
    char t[] = "/tmp/xXXXXXX";
    mkstemp(t);
}

void mkdtemp_ok(void) {
    mkdtemp("/tmp/xXXXXXX");
}
