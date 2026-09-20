void eventfd_write_unenc_bad(void) {
    eventfd_write();
}

int eventfd_write_unenc_ok(int n) {
    return n;
}
