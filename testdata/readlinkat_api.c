int readlinkat(int dirfd, const char *pathname, char *buf, unsigned bufsiz);

void readlinkat_bad(void) {
    readlinkat(0, "x", 0, 0);
}

void readlinkat_ok(void) {
    if (readlinkat(0, "x", 0, 0) != 0)
        return;
}
