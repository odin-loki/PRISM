char *devname(int dev, int type);

void devname_bad(void) {
    devname(0, 0);
}

void devname_ok(void) {
    if (devname(0, 0) == 0)
        return;
}
