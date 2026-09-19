int unlink(const char *path);
int remove(const char *path);

void unlink_bad(void) {
    unlink("x");
}

void unlink_ok(void) {
    if (unlink("x") != 0)
        return;
}
