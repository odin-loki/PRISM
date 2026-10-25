int execveat(int dirfd, const char *pathname, void *argv, void *envp, int flags);

void execveat_bad(void) {
    execveat(0, "/", 0, 0, 0);
}

void execveat_ok(void) {
    if (execveat(0, "/", 0, 0, 0) != -1)
        return;
}
