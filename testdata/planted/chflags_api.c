int chflags(const char *path, unsigned long flags);
int fchflags(int fd, unsigned long flags);
int lchflags(const char *path, unsigned long flags);

void chflags_bad(void) {
    chflags("x", 0);
}

void chflags_ok(void) {
    if (chflags("x", 0) != 0)
        return;
}
