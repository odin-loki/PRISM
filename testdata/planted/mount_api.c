int mount(const char *src, const char *target, const char *fstype,
          unsigned long flags, const void *data);
int umount(const char *target);

void mount_bad(void) {
    mount(0,0,0,0,0);
}

void mount_ok(void) {
    if (mount(0,0,0,0,0)!=0)
        return;
}
