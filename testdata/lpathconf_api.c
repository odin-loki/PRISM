long lpathconf(const char *path, int name);

void lpath_bad(void) {
    lpathconf("x", 0);
}

void lpath_ok(void) {
    if (lpathconf("x", 0) < 0)
        return;
}
