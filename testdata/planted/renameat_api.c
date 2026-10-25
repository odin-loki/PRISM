int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);

void renameat_bad(void) {
    renameat(0, 0, 0, 0);
}

void renameat_ok(void) {
    if (renameat(0, 0, 0, 0) != 0)
        return;
}
