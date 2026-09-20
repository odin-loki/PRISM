void io_uring_enter_unenc_bad(void) {
    io_uring_enter();
}

int io_uring_enter_unenc_ok(int n) {
    return n;
}
