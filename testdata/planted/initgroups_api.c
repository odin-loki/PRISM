int initgroups(const char *user, unsigned gid);

void initgroups_bad(void) {
    initgroups("x",0);
}

void initgroups_ok(void) {
    if (initgroups("x",0)!=0)
        return;
}
