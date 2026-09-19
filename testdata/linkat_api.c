int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);

void linkat_bad(void) {
    linkat(0,0,0,0,0);
}

void linkat_ok(void) {
    if (linkat(0,0,0,0,0)!=-1)
        return;
}
