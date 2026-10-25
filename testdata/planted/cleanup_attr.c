int cleanup_unenc_bad(int n) {
    int x __attribute__((cleanup(fn)));
    return n;
}

int cleanup_ok(int n) {
    return n;
}
