int posix_fallocate(int fd, long offset, long len);
int fallocate(int fd, int mode, long offset, long len);

void fallocate_bad(void) {
    posix_fallocate(0,0,0);
}

void fallocate_ok(void) {
    if (posix_fallocate(0,0,0)!=0)
        return;
}
