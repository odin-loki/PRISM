int swait_unenc_bad(int n) {
    sync_wait();
    return n;
}

int swait_unenc_ok(int n) {
    return n;
}
