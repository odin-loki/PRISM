typedef struct _IO_FILE FILE;
FILE *fmemopen(void *buf, unsigned long size, const char *mode);
FILE *open_memstream(char **ptr, unsigned long *sizeloc);
int fclose(FILE *f);

void fmemopen_bad(void) {
    FILE *f=fmemopen(0,0,0);
    fclose(f);
}

void fmemopen_ok(void) {
    FILE *f=fmemopen(0,0,0);
    if (!f)
        return;
    fclose(f);
}
