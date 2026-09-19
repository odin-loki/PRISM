int format_unenc_bad(int n) {
    (void)std::format(n);
    return n;
}

int format_unenc_ok(int n) {
    return n;
}
