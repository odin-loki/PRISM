int renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned flags);

void renameat2_bad(void) {
    renameat2(0, 0, 0, 0, 0);
}

void renameat2_ok(void) {
    if (renameat2(0, 0, 0, 0, 0) != 0)
        return;
}
