int texscan_unenc_bad(int n) {
    transform_exclusive_scan();
    return n;
}

int texscan_unenc_ok(int n) {
    return n;
}
