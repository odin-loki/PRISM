int access(const char *path, int mode);

void access_bad(void) {
    access("x",0);
}

void access_ok(void) {
    if (access("x",0)!=0)
        return;
}
