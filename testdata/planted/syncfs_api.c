int syncfs(int fd);

void syncfs_bad(void) {
    syncfs(0);
}

void syncfs_ok(void) {
    if (syncfs(0)!=0)
        return;
}
