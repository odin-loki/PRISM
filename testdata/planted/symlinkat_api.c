int symlinkat(const char *target, int newdirfd, const char *linkpath);

void symlinkat_bad(void) {
    symlinkat("a", 0, "b");
}

void symlinkat_ok(void) {
    if (symlinkat("a", 0, "b") != 0)
        return;
}
