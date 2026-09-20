int offsetof_unenc_bad(int n) {
    (void)n;
    return offsetof(struct S, f);
}

int offsetof_unenc_ok(int n) {
    return n;
}
