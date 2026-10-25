void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);

void mmap_check_bad(void) {
    char *p = mmap(0, 4096, 3, 1, -1, 0);
    p[0] = 1;
}

void mmap_check_ok(void) {
    char *p = mmap(0, 4096, 3, 1, -1, 0);
    if (p != (void*)-1)
        p[0] = 1;
}
