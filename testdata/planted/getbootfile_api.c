const char *getbootfile(void);

void bootfile_bad(void) {
    getbootfile();
}

void bootfile_ok(void) {
    if (getbootfile() == 0)
        return;
}
