int nmount(void *iov, unsigned niov, int flags);

void nmount_bad(void) {
    nmount(0, 0, 0);
}

void nmount_ok(void) {
    if (nmount(0, 0, 0) != 0)
        return;
}
