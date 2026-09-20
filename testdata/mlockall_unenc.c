void mlockall_unenc_bad(void) {
    mlockall();
}

int mlockall_unenc_ok(int n) {
    return n;
}
