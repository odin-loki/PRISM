int symlink(const char *a, const char *b);
int readlink(const char *path, char *buf, unsigned n);

void symlink_bad(void) {
    symlink("a", "b");
}

void symlink_ok(void) {
    if (symlink("a", "b") != 0)
        return;
}
