int extattr_set_file(const char *path, int ns, const char *name,
                     const void *data, unsigned n);

void extattr_bad(void) {
    extattr_set_file("x", 0, "u", 0, 0);
}

void extattr_ok(void) {
    if (extattr_set_file("x", 0, "u", 0, 0) < 0)
        return;
}
