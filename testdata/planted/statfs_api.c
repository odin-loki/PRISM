int statfs(const char *path, void *buf);

void statfs_bad(void) {
    statfs("x",0);
}

void statfs_ok(void) {
    if (statfs("x",0)!=0)
        return;
}
