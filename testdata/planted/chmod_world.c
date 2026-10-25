int chmod(const char *path, unsigned mode);
int fchmod(int fd, unsigned mode);

void chmod_world_bad(void) {
    chmod("/tmp/x", 0777);
}

void chmod_ok(void) {
    chmod("/tmp/x", 0755);
}
