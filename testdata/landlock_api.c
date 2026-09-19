int landlock_create_ruleset(const void *attr, unsigned size, unsigned flags);
int close(int fd);

void landlock_bad(void) {
    int fd=landlock_create_ruleset(0,0,0);
    close(fd);
}

void landlock_ok(void) {
    int fd=landlock_create_ruleset(0,0,0);
    if (fd<0)
        return;
    close(fd);
}
