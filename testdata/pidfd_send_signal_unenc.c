void pidfd_send_signal_unenc_bad(void) {
    pidfd_send_signal();
}

int pidfd_send_signal_unenc_ok(int n) {
    return n;
}
