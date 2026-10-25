int eaccess(const char *path, int mode);

void eaccess_bad(void) {
    eaccess("x", 0);
}

void eaccess_ok(void) {
    if (eaccess("x", 0) != 0)
        return;
}
