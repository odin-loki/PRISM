int span_unenc_bad(int n) {
    std::span<int> s;
    (void)s;
    return n;
}

int span_unenc_ok(int n) {
    return n;
}
