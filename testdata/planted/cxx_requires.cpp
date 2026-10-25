int requires_unenc_bad(int n) {
    (void)(requires (n > 0));
    return n;
}

int requires_ok(int n) {
    return n;
}
