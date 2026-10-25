int getgrouplist(const char *name, int basegid, int *groups, int *ngroups);

void ggl_bad(void) {
    getgrouplist("x", 0, 0, 0);
}

void ggl_ok(void) {
    if (getgrouplist("x", 0, 0, 0) != 0)
        return;
}
