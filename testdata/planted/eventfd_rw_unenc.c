void eventfd_rw_unenc_bad(void) {
    eventfd_read();
}

int eventfd_rw_unenc_ok(int n) {
    return n;
}
