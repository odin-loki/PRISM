int optional_unenc_bad(int n) {
    optional<int> o;
    (void)o;
    return n;
}

int optional_unenc_ok(int n) {
    return n;
}
