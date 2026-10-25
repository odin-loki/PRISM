int ranges_unenc_bad(int n) {
    std::views::iota(0, n);
    return n;
}

int ranges_unenc_ok(int n) {
    return n;
}
