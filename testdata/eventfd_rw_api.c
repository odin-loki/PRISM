int eventfd_read(int fd, unsigned long long *value);

void efd_rw_bad(void) {
    eventfd_read(0, 0);
}

void efd_rw_ok(void) {
    if (eventfd_read(0, 0) != 0)
        return;
}
