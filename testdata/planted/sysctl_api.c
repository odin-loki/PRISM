int sysctl(int *name, unsigned namelen, void *oldp, void *oldlenp, void *newp, unsigned newlen);

void sysctl_bad(void) {
    sysctl(0, 0, 0, 0, 0, 0);
}

void sysctl_ok(void) {
    if (sysctl(0, 0, 0, 0, 0, 0) != 0)
        return;
}
