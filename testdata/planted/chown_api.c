int chown(const char *path, int uid, int gid);
int fchown(int fd, int uid, int gid);
int lchown(const char *path, int uid, int gid);

void chown_bad(void) {
    chown("x", 0, 0);
}

void chown_ok(void) {
    if (chown("x", 0, 0) != 0)
        return;
}
