int mofn_unenc_bad(int n) {
    std::move_only_function<int()> f;
    return n;
}

int mofn_unenc_ok(int n) {
    return n;
}
