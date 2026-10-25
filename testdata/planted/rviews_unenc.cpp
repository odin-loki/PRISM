int rviews_unenc_bad(int n) {
    std::views::all(n);
    return n;
}

int rviews_unenc_ok(int n) {
    return n;
}
