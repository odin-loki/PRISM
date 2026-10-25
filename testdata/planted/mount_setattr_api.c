int mount_setattr(int dirfd, const char *path, unsigned flags,
                  void *attr, unsigned long size);

void mntsa_bad(void) {
    mount_setattr(0, 0, 0, 0, 0);
}

void mntsa_ok(void) {
    if (mount_setattr(0, 0, 0, 0, 0)!=-1)
        return;
}
