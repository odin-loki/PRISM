int glob(const char *pattern, int flags, void *errfunc, void *pglob);

void glob_bad(void) {
    glob("*",0,0,0);
}

void glob_ok(void) {
    int g;
    if (glob("*",0,0,&g)!=0)
        return;
}
