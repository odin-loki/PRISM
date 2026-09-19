struct stat { int st_mode; };
int stat(const char *path, struct stat *st);

void stat_bad(void) {
    stat("x",0);
}

void stat_ok(void) {
    struct stat st;
    if (stat("x",&st)!=0)
        return;
}
