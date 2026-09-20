int alignof_unenc_bad(int n) {
    (void)n;
    return (int)alignof(int);
}

int alignof_unenc_ok(int n) {
    return n;
}
