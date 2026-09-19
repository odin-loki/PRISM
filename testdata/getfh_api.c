int getfh(const char *path, void *fhp);
int fhopen(const void *fhp, int flags);
int fhstat(const void *fhp, void *sb);
int fhstatfs(const void *fhp, void *buf);
int getfhat(int fd, char *path, void *fhp, int flag);

void getfh_bad(void) {
    getfh("x", 0);
}

void getfh_ok(void) {
    if (getfh("x", 0) != 0)
        return;
}
