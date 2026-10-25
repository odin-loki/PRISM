typedef struct _IO_FILE FILE;

FILE *popen(const char *, const char *);
int pclose(FILE *);

void popen_bad(void) {
    FILE *f = popen("true", "r");
    return;
}

void popen_ok(void) {
    FILE *f = popen("true", "r");
    if (f)
        pclose(f);
}
