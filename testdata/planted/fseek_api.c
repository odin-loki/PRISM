int fseek(void *stream, long off, int whence);

void fseek_bad(void) {
    fseek(0,0,0);
}

void fseek_ok(void) {
    if (fseek(0,0,0)!=0)
        return;
}
